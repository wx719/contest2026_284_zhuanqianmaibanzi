/****************************************************************************
 * apps/app/velapoka/velapoka_network.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <netutils/netlib.h>

#include "velapoka_network.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if !defined(CONFIG_NET) || !defined(CONFIG_NET_TCP) || \
    !defined(CONFIG_NET_SOCKOPTS) || !defined(CONFIG_NET_TCPBACKLOG) || \
    !defined(CONFIG_NETUTILS_NETLIB)
#  error "VelaPoka network export requires TCP backlog, sockopts and netlib"
#endif

#define VELAPOKA_NETWORK_INTERFACE       "eth0"
#define VELAPOKA_NETWORK_REQUEST_SIZE    768
#define VELAPOKA_NETWORK_HEADER_SIZE     384
#define VELAPOKA_NETWORK_POLL_TIMEOUT_MS 250
#define VELAPOKA_NETWORK_IO_TIMEOUT_SEC  2
#define VELAPOKA_NETWORK_BACKLOG         2

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct velapoka_network_status_s
{
  enum velapoka_state_e state;
  unsigned int threshold_percent;
  bool camera_online;
  bool storage_online;
  bool reference_ready;
};

struct velapoka_network_s
{
  pthread_mutex_t lock;
  pthread_t thread;
  FAR struct velapoka_storage_s *storage;
  struct velapoka_network_status_s status;
  int last_error;
  bool online;
  bool stop;
  bool thread_started;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int velapoka_network_errno(void)
{
  return errno > 0 ? -errno : -EIO;
}

static bool velapoka_network_should_stop(
  FAR struct velapoka_network_s *network)
{
  bool stop;

  pthread_mutex_lock(&network->lock);
  stop = network->stop;
  pthread_mutex_unlock(&network->lock);
  return stop;
}

static void velapoka_network_set_result(
  FAR struct velapoka_network_s *network, bool online, int error)
{
  pthread_mutex_lock(&network->lock);
  network->online = online;
  network->last_error = error;
  pthread_mutex_unlock(&network->lock);
}

static int velapoka_network_configure_interface(void)
{
  struct in_addr address;

  address.s_addr = htonl(CONFIG_EXAMPLES_VELAPOKA_NETWORK_IPADDR);
  if (netlib_set_ipv4addr(VELAPOKA_NETWORK_INTERFACE, &address) < 0)
    {
      return velapoka_network_errno();
    }

  address.s_addr = htonl(CONFIG_EXAMPLES_VELAPOKA_NETWORK_NETMASK);
  if (netlib_set_ipv4netmask(VELAPOKA_NETWORK_INTERFACE, &address) < 0)
    {
      return velapoka_network_errno();
    }

  address.s_addr = htonl(CONFIG_EXAMPLES_VELAPOKA_NETWORK_ROUTER);
  if (netlib_set_dripv4addr(VELAPOKA_NETWORK_INTERFACE, &address) < 0)
    {
      return velapoka_network_errno();
    }

  if (netlib_ifup(VELAPOKA_NETWORK_INTERFACE) < 0)
    {
      return velapoka_network_errno();
    }

  return OK;
}

static void velapoka_network_format_ip(FAR char *buffer, size_t size)
{
  uint32_t address = CONFIG_EXAMPLES_VELAPOKA_NETWORK_IPADDR;

  snprintf(buffer, size, "%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32,
           (address >> 24) & 0xff, (address >> 16) & 0xff,
           (address >> 8) & 0xff, address & 0xff);
}

static int velapoka_network_send_all(int fd, FAR const void *data,
                                     size_t size)
{
  FAR const uint8_t *buffer = data;
  size_t sent = 0;

  while (sent < size)
    {
      ssize_t ret = send(fd, buffer + sent, size - sent, MSG_NOSIGNAL);

      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (ret == 0)
        {
          return -EPIPE;
        }

      sent += ret;
    }

  return OK;
}

static int velapoka_network_send_response(
  int fd, int status, FAR const char *reason, FAR const char *content_type,
  FAR const void *body, size_t body_size)
{
  char header[VELAPOKA_NETWORK_HEADER_SIZE];
  int length;
  int ret;

  length = snprintf(header, sizeof(header),
                    "HTTP/1.1 %d %s\r\n"
                    "Server: VelaPoka\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %zu\r\n"
                    "Cache-Control: no-store\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n",
                    status, reason, content_type, body_size);
  if (length <= 0 || (size_t)length >= sizeof(header))
    {
      return -EOVERFLOW;
    }

  ret = velapoka_network_send_all(fd, header, length);
  if (ret == OK && body_size > 0)
    {
      ret = velapoka_network_send_all(fd, body, body_size);
    }

  return ret;
}

static int velapoka_network_send_error(int fd, int status,
                                       FAR const char *reason,
                                       FAR const char *message)
{
  char body[160];
  int length;

  length = snprintf(body, sizeof(body),
                    "{\"error\":\"%s\",\"status\":%d}\n",
                    message, status);
  if (length <= 0 || (size_t)length >= sizeof(body))
    {
      return -EOVERFLOW;
    }

  return velapoka_network_send_response(fd, status, reason,
                                        "application/json", body, length);
}

static int velapoka_network_send_status(
  FAR struct velapoka_network_s *network, int fd)
{
  struct velapoka_history_s history[VELAPOKA_HISTORY_COUNT];
  struct velapoka_network_status_s status;
  char body[512];
  char ip[INET_ADDRSTRLEN];
  uint32_t latest_id = 0;
  unsigned int history_count;
  int length;

  pthread_mutex_lock(&network->lock);
  status = network->status;
  pthread_mutex_unlock(&network->lock);

  history_count = velapoka_storage_get_history(
    network->storage, history, VELAPOKA_HISTORY_COUNT);
  if (history_count > 0)
    {
      latest_id = history[0].id;
    }

  velapoka_network_format_ip(ip, sizeof(ip));

  length = snprintf(
    body, sizeof(body),
    "{\"service\":\"VelaPoka\",\"state\":\"%s\","
    "\"interface\":\"%s\",\"ip\":\"%s\",\"port\":%u,"
    "\"camera_online\":%s,\"storage_online\":%s,"
    "\"reference_ready\":%s,\"threshold_percent\":%u,"
    "\"latest_record_id\":%" PRIu32 ",\"history_count\":%u}\n",
    velapoka_state_name(status.state), VELAPOKA_NETWORK_INTERFACE, ip,
    CONFIG_EXAMPLES_VELAPOKA_NETWORK_PORT,
    status.camera_online ? "true" : "false",
    status.storage_online ? "true" : "false",
    status.reference_ready ? "true" : "false",
    status.threshold_percent, latest_id, history_count);
  if (length <= 0 || (size_t)length >= sizeof(body))
    {
      return velapoka_network_send_error(fd, 500, "Internal Server Error",
                                         "status serialization failed");
    }

  return velapoka_network_send_response(fd, 200, "OK",
                                        "application/json", body, length);
}

static int velapoka_network_send_export(
  FAR struct velapoka_network_s *network, int fd,
  FAR const char *relative_path, FAR const char *content_type)
{
  FAR uint8_t *data;
  size_t size;
  int ret;

  if (network->storage == NULL)
    {
      return velapoka_network_send_error(fd, 503,
                                         "Service Unavailable",
                                         "storage export unavailable");
    }

  ret = velapoka_storage_read_export(network->storage, relative_path,
                                     &data, &size);
  if (ret == -ENOENT && strcmp(relative_path, "results.jsonl") == 0)
    {
      return velapoka_network_send_response(fd, 200, "OK", content_type,
                                            NULL, 0);
    }

  if (ret == -ENOENT)
    {
      return velapoka_network_send_error(fd, 404, "Not Found",
                                         "record file not found");
    }

  if (ret == -EINVAL)
    {
      return velapoka_network_send_error(fd, 404, "Not Found",
                                         "record file not found");
    }

  if (ret < 0)
    {
      return velapoka_network_send_error(fd, 503,
                                         "Service Unavailable",
                                         "storage export unavailable");
    }

  ret = velapoka_network_send_response(fd, 200, "OK", content_type,
                                       data, size);
  free(data);
  return ret;
}

static int velapoka_network_parse_path(FAR char *request, size_t size,
                                       FAR char **path)
{
  FAR char *end;
  FAR char *query;

  request[size] = '\0';
  if (strncmp(request, "GET ", 4) != 0)
    {
      return -ENOTSUP;
    }

  *path = request + 4;
  end = strchr(*path, ' ');
  if (end == NULL || end == *path)
    {
      return -EBADMSG;
    }

  *end = '\0';
  query = strchr(*path, '?');
  if (query != NULL)
    {
      *query = '\0';
    }

  return OK;
}

static int velapoka_network_read_request(int fd, FAR char *request,
                                         size_t capacity,
                                         FAR size_t *request_size)
{
  size_t used = 0;

  while (used < capacity)
    {
      ssize_t ret = recv(fd, request + used, capacity - used, 0);

      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (ret == 0)
        {
          break;
        }

      used += ret;
      if ((used >= 4 && strstr(request, "\r\n\r\n") != NULL) ||
          (used >= 2 && strstr(request, "\n\n") != NULL))
        {
          break;
        }
    }

  *request_size = used;
  return used > 0 ? OK : -ECONNRESET;
}

static int velapoka_network_handle_client(
  FAR struct velapoka_network_s *network, int fd)
{
  char request[VELAPOKA_NETWORK_REQUEST_SIZE + 1];
  FAR char *path;
  size_t request_size = 0;
  int ret;

  memset(request, 0, sizeof(request));
  ret = velapoka_network_read_request(fd, request,
                                      VELAPOKA_NETWORK_REQUEST_SIZE,
                                      &request_size);
  if (ret < 0)
    {
      return ret;
    }

  ret = velapoka_network_parse_path(request, request_size, &path);
  if (ret == -ENOTSUP)
    {
      return velapoka_network_send_error(fd, 405, "Method Not Allowed",
                                         "only GET is supported");
    }
  else if (ret < 0)
    {
      return velapoka_network_send_error(fd, 400, "Bad Request",
                                         "invalid HTTP request");
    }

  if (strcmp(path, "/") == 0)
    {
      static const char body[] =
        "{\"service\":\"VelaPoka\",\"endpoints\":["
        "\"/api/status\",\"/api/results\","
        "\"/files/fail/fail_XXXXXXXX.bmp\"]}\n";

      return velapoka_network_send_response(fd, 200, "OK",
                                            "application/json", body,
                                            sizeof(body) - 1);
    }

  if (strcmp(path, "/api/status") == 0)
    {
      return velapoka_network_send_status(network, fd);
    }

  if (strcmp(path, "/api/results") == 0)
    {
      return velapoka_network_send_export(network, fd, "results.jsonl",
                                          "application/x-ndjson");
    }

  if (strncmp(path, "/files/", 7) == 0)
    {
      return velapoka_network_send_export(network, fd, path + 7,
                                          "image/bmp");
    }

  return velapoka_network_send_error(fd, 404, "Not Found",
                                     "endpoint not found");
}

static FAR void *velapoka_network_thread(FAR void *arg)
{
  FAR struct velapoka_network_s *network = arg;
  struct sockaddr_in address;
  struct pollfd descriptor;
  char ip[INET_ADDRSTRLEN];
  int listen_fd;
  int option = 1;
  int ret;

  ret = velapoka_network_configure_interface();
  if (ret < 0)
    {
      fprintf(stderr, "VelaPoka network: %s setup failed: %d\n",
              VELAPOKA_NETWORK_INTERFACE, ret);
      velapoka_network_set_result(network, false, ret);
      return NULL;
    }

  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0)
    {
      ret = -errno;
      goto out;
    }

  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(CONFIG_EXAMPLES_VELAPOKA_NETWORK_PORT);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listen_fd, (FAR struct sockaddr *)&address,
           sizeof(address)) < 0)
    {
      ret = -errno;
      close(listen_fd);
      goto out;
    }

  if (listen(listen_fd, VELAPOKA_NETWORK_BACKLOG) < 0)
    {
      ret = -errno;
      close(listen_fd);
      goto out;
    }

  velapoka_network_set_result(network, true, OK);
  velapoka_network_format_ip(ip, sizeof(ip));
  printf("VelaPoka network: http://%s:%u ready on %s\n", ip,
         CONFIG_EXAMPLES_VELAPOKA_NETWORK_PORT,
         VELAPOKA_NETWORK_INTERFACE);

  descriptor.fd = listen_fd;
  descriptor.events = POLLIN;
  while (!velapoka_network_should_stop(network))
    {
      descriptor.revents = 0;
      ret = poll(&descriptor, 1, VELAPOKA_NETWORK_POLL_TIMEOUT_MS);
      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          ret = -errno;
          break;
        }

      if (ret > 0 && (descriptor.revents & POLLIN) != 0)
        {
          struct timeval timeout;
          int client_fd = accept(listen_fd, NULL, NULL);

          if (client_fd < 0)
            {
              if (errno == EINTR)
                {
                  continue;
                }

              ret = -errno;
              break;
            }

          timeout.tv_sec = VELAPOKA_NETWORK_IO_TIMEOUT_SEC;
          timeout.tv_usec = 0;
          if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                         &timeout, sizeof(timeout)) < 0 ||
              setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
                         &timeout, sizeof(timeout)) < 0)
            {
              fprintf(stderr,
                      "VelaPoka network: client timeout setup failed: %d\n",
                      errno);
              close(client_fd);
              continue;
            }

          velapoka_network_handle_client(network, client_fd);
          close(client_fd);
        }
      else if (ret > 0 &&
               (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
          ret = -EIO;
          break;
        }
    }

  close(listen_fd);
  if (velapoka_network_should_stop(network))
    {
      ret = OK;
    }

out:
  velapoka_network_set_result(network, false, ret);
  if (ret < 0)
    {
      fprintf(stderr, "VelaPoka network: server stopped: %d\n", ret);
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velapoka_network_start(
  FAR struct velapoka_network_s **network_out,
  FAR struct velapoka_storage_s *storage)
{
  FAR struct velapoka_network_s *network;
  struct sched_param param;
  pthread_attr_t attr;
  int ret;

  if (network_out == NULL)
    {
      return -EINVAL;
    }

  *network_out = NULL;
  network = calloc(1, sizeof(*network));
  if (network == NULL)
    {
      return -ENOMEM;
    }

  ret = pthread_mutex_init(&network->lock, NULL);
  if (ret != 0)
    {
      free(network);
      return -ret;
    }

  network->storage = storage;
  network->status.state = VELAPOKA_STATE_STARTUP;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr,
                            CONFIG_EXAMPLES_VELAPOKA_NETWORK_STACKSIZE);
  memset(&param, 0, sizeof(param));
  param.sched_priority = CONFIG_EXAMPLES_VELAPOKA_NETWORK_PRIORITY;
  pthread_attr_setschedparam(&attr, &param);
  ret = pthread_create(&network->thread, &attr,
                       velapoka_network_thread, network);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      pthread_mutex_destroy(&network->lock);
      free(network);
      return -ret;
    }

  network->thread_started = true;
  *network_out = network;
  return OK;
}

int velapoka_network_stop(FAR struct velapoka_network_s *network)
{
  int ret;

  if (network == NULL)
    {
      return OK;
    }

  pthread_mutex_lock(&network->lock);
  network->stop = true;
  pthread_mutex_unlock(&network->lock);
  if (network->thread_started)
    {
      pthread_join(network->thread, NULL);
    }

  ret = network->last_error;
  pthread_mutex_destroy(&network->lock);
  free(network);
  return ret;
}

void velapoka_network_update(
  FAR struct velapoka_network_s *network, enum velapoka_state_e state,
  bool camera_online, bool storage_online, bool reference_ready,
  unsigned int threshold_percent)
{
  if (network == NULL)
    {
      return;
    }

  pthread_mutex_lock(&network->lock);
  network->status.state = state;
  network->status.camera_online = camera_online;
  network->status.storage_online = storage_online;
  network->status.reference_ready = reference_ready;
  network->status.threshold_percent = threshold_percent;
  pthread_mutex_unlock(&network->lock);
}

bool velapoka_network_online(FAR struct velapoka_network_s *network)
{
  bool online;

  if (network == NULL)
    {
      return false;
    }

  pthread_mutex_lock(&network->lock);
  online = network->online;
  pthread_mutex_unlock(&network->lock);
  return online;
}

int velapoka_network_last_error(FAR struct velapoka_network_s *network)
{
  int error;

  if (network == NULL)
    {
      return -ENODEV;
    }

  pthread_mutex_lock(&network->lock);
  error = network->last_error;
  pthread_mutex_unlock(&network->lock);
  return error;
}
