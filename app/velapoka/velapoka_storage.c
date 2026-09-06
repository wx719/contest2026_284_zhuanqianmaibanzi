/****************************************************************************
 * apps/app/velapoka/velapoka_storage.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <syslog.h>
#include <unistd.h>

#include "velapoka_storage.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VELAPOKA_STORAGE_ROOT          "velapoka"
#define VELAPOKA_STORAGE_FAIL_DIR      "fail"
#define VELAPOKA_STORAGE_TEMPLATE      "template.bin"
#define VELAPOKA_STORAGE_TEMPLATE_TMP  "template.tmp"
#define VELAPOKA_STORAGE_CONFIG        "config.json"
#define VELAPOKA_STORAGE_CONFIG_TMP    "config.tmp"
#define VELAPOKA_STORAGE_RESULTS       "results.jsonl"
#define VELAPOKA_STORAGE_MAGIC         UINT32_C(0x31504b56)
#define VELAPOKA_STORAGE_VERSION       1
#define VELAPOKA_STORAGE_HEADER_SIZE   36
#define VELAPOKA_STORAGE_PATH_SIZE     160
#define VELAPOKA_STORAGE_JSON_SIZE     1024
#define VELAPOKA_BMP_HEADER_SIZE       1078
#define VELAPOKA_STORAGE_QUEUE_DEPTH   4
#define VELAPOKA_STORAGE_EXPORT_MAX    (4 * 1024 * 1024)

#if !defined(CONFIG_FAT_LFN) || CONFIG_FAT_MAXFNAME < 17
#  error "VelaPoka storage requires FAT long filenames of at least 17 bytes"
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct velapoka_storage_request_s
{
  struct velapoka_result_s result;
  FAR uint8_t *image;
  uint32_t id;
  unsigned int threshold;
  bool has_image;
};

struct velapoka_storage_s
{
  pthread_mutex_t lock;
  pthread_mutex_t io_lock;
  pthread_cond_t cond;
  pthread_t thread;
  FAR uint8_t *reference;
  struct velapoka_storage_request_s
    requests[VELAPOKA_STORAGE_QUEUE_DEPTH];
  struct velapoka_history_s history[VELAPOKA_HISTORY_COUNT];
  uint32_t next_id;
  unsigned int reference_threshold;
  unsigned int history_count;
  unsigned int request_head;
  unsigned int request_tail;
  unsigned int request_count;
  int last_error;
  bool online;
  bool reference_pending;
  bool reference_busy;
  bool stop;
  bool thread_started;
  bool mounted_here;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void velapoka_put_le16(FAR uint8_t *buffer, uint16_t value)
{
  buffer[0] = value & 0xff;
  buffer[1] = value >> 8;
}

static void velapoka_put_le32(FAR uint8_t *buffer, uint32_t value)
{
  buffer[0] = value & 0xff;
  buffer[1] = (value >> 8) & 0xff;
  buffer[2] = (value >> 16) & 0xff;
  buffer[3] = value >> 24;
}

static uint16_t velapoka_get_le16(FAR const uint8_t *buffer)
{
  return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

static uint32_t velapoka_get_le32(FAR const uint8_t *buffer)
{
  return (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
         ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
}

static uint32_t velapoka_crc32(FAR const uint8_t *data, size_t size)
{
  uint32_t crc = UINT32_MAX;
  size_t i;

  for (i = 0; i < size; i++)
    {
      unsigned int bit;

      crc ^= data[i];
      for (bit = 0; bit < 8; bit++)
        {
          crc = (crc >> 1) ^
                (UINT32_C(0xedb88320) & (0 - (crc & 1)));
        }
    }

  return ~crc;
}

static int velapoka_path(FAR char *path, size_t size,
                         FAR const char *name)
{
  int length;

  length = snprintf(path, size, "%s/%s/%s",
                    CONFIG_EXAMPLES_VELAPOKA_STORAGE_MOUNTPOINT,
                    VELAPOKA_STORAGE_ROOT, name);
  return length > 0 && (size_t)length < size ? OK : -ENAMETOOLONG;
}

static bool velapoka_export_path_valid(FAR const char *relative_path)
{
  FAR const char *name;
  unsigned int i;

  if (strcmp(relative_path, VELAPOKA_STORAGE_RESULTS) == 0)
    {
      return true;
    }

  name = relative_path;
  if (strncmp(name, VELAPOKA_STORAGE_FAIL_DIR "/fail_", 10) != 0)
    {
      return false;
    }

  name += 10;
  for (i = 0; i < 8; i++)
    {
      if (name[i] < '0' || name[i] > '9')
        {
          return false;
        }
    }

  return strcmp(name + 8, ".bmp") == 0;
}

static int velapoka_write_all(int fd, FAR const uint8_t *buffer,
                              size_t size)
{
  size_t written = 0;

  while (written < size)
    {
      ssize_t ret = write(fd, buffer + written, size - written);

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
          return -EIO;
        }

      written += ret;
    }

  return OK;
}

static int velapoka_read_all(int fd, FAR uint8_t *buffer, size_t size)
{
  size_t received = 0;

  while (received < size)
    {
      ssize_t ret = read(fd, buffer + received, size - received);

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
          return -EIO;
        }

      received += ret;
    }

  return OK;
}

static int velapoka_sync_close(int fd, int ret)
{
  if (ret == OK && fsync(fd) < 0)
    {
      ret = -errno;
    }

  if (close(fd) < 0 && ret == OK)
    {
      ret = -errno;
    }

  return ret;
}

static int velapoka_replace(FAR const char *temporary,
                            FAR const char *destination)
{
  if (rename(temporary, destination) < 0)
    {
      int ret = -errno;
      unlink(temporary);
      return ret;
    }

  return OK;
}

static int velapoka_mkdir(FAR const char *path)
{
  if (mkdir(path, 0775) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  return OK;
}

static int velapoka_mkdir_all(FAR const char *path)
{
  char partial[VELAPOKA_STORAGE_PATH_SIZE];
  FAR char *separator;
  size_t length;
  int ret;

  length = strlen(path);
  if (length == 0 || length >= sizeof(partial) || path[0] != '/')
    {
      return -EINVAL;
    }

  memcpy(partial, path, length + 1);
  separator = partial + 1;
  while ((separator = strchr(separator, '/')) != NULL)
    {
      *separator = '\0';
      ret = velapoka_mkdir(partial);
      *separator = '/';
      if (ret < 0)
        {
          return ret;
        }

      separator++;
    }

  return velapoka_mkdir(partial);
}

static bool velapoka_storage_is_mounted(void)
{
  struct statfs filesystem;

  return statfs(CONFIG_EXAMPLES_VELAPOKA_STORAGE_MOUNTPOINT,
                &filesystem) == 0 &&
         filesystem.f_type == MSDOS_SUPER_MAGIC;
}

static int velapoka_storage_mount(FAR bool *mounted_here)
{
  char root[VELAPOKA_STORAGE_PATH_SIZE];
  char fail[VELAPOKA_STORAGE_PATH_SIZE];
  int error;
  int length;
  int ret;

  *mounted_here = false;
  ret = velapoka_mkdir_all(CONFIG_EXAMPLES_VELAPOKA_STORAGE_MOUNTPOINT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "VelaPoka storage: mkdir %s failed: %d\n",
             CONFIG_EXAMPLES_VELAPOKA_STORAGE_MOUNTPOINT, ret);
      return ret;
    }

  if (!velapoka_storage_is_mounted())
    {
      if (mount(CONFIG_EXAMPLES_VELAPOKA_STORAGE_DEVPATH,
                CONFIG_EXAMPLES_VELAPOKA_STORAGE_MOUNTPOINT,
                "vfat", 0, NULL) < 0)
        {
          error = errno;
          if ((error != EBUSY && error != ENOTDIR) ||
              !velapoka_storage_is_mounted())
            {
              syslog(LOG_ERR,
                     "VelaPoka storage: mount %s at %s failed: %d\n",
                     CONFIG_EXAMPLES_VELAPOKA_STORAGE_DEVPATH,
                     CONFIG_EXAMPLES_VELAPOKA_STORAGE_MOUNTPOINT, error);
              return -error;
            }
        }
      else
        {
          *mounted_here = true;
        }
    }

  length = snprintf(root, sizeof(root), "%s/%s",
                    CONFIG_EXAMPLES_VELAPOKA_STORAGE_MOUNTPOINT,
                    VELAPOKA_STORAGE_ROOT);
  if (length <= 0 || (size_t)length >= sizeof(root))
    {
      return -ENAMETOOLONG;
    }

  ret = velapoka_mkdir(root);
  if (ret < 0)
    {
      return ret;
    }

  length = snprintf(fail, sizeof(fail), "%s/%s", root,
                    VELAPOKA_STORAGE_FAIL_DIR);
  if (length <= 0 || (size_t)length >= sizeof(fail))
    {
      return -ENAMETOOLONG;
    }

  return velapoka_mkdir(fail);
}

static void velapoka_history_push(
  FAR struct velapoka_storage_s *storage, uint32_t id, bool pass,
  uint16_t score_tenths)
{
  unsigned int move;

  move = storage->history_count < VELAPOKA_HISTORY_COUNT ?
         storage->history_count : VELAPOKA_HISTORY_COUNT - 1;
  while (move > 0)
    {
      storage->history[move] = storage->history[move - 1];
      move--;
    }

  storage->history[0].id = id;
  storage->history[0].pass = pass;
  storage->history[0].score_tenths = score_tenths;
  if (storage->history_count < VELAPOKA_HISTORY_COUNT)
    {
      storage->history_count++;
    }
}

static int velapoka_storage_load_history(
  FAR struct velapoka_storage_s *storage)
{
  char path[VELAPOKA_STORAGE_PATH_SIZE];
  char line[VELAPOKA_STORAGE_JSON_SIZE];
  FILE *stream;
  unsigned int loaded = 0;
  unsigned int duplicates = 0;
  unsigned int skipped = 0;
  uint32_t previous_id = 0;
  bool have_previous = false;
  int ret;

  ret = velapoka_path(path, sizeof(path), VELAPOKA_STORAGE_RESULTS);
  if (ret < 0)
    {
      return ret;
    }

  stream = fopen(path, "r");
  if (stream == NULL)
    {
      return errno == ENOENT ? OK : -errno;
    }

  while (fgets(line, sizeof(line), stream) != NULL)
    {
      char result[5];
      uint32_t id;
      unsigned int score;

      if (sscanf(line, "{\"id\":%" SCNu32
                 ",\"frame\":%*u,\"result\":\"%4s\""
                 ",\"score_tenths\":%u", &id, result, &score) == 3)
        {
          if (have_previous && id <= previous_id)
            {
              duplicates++;
              continue;
            }

          velapoka_history_push(storage, id,
                                strcmp(result, "PASS") == 0,
                                score > UINT16_MAX ? UINT16_MAX : score);
          loaded++;
          previous_id = id;
          have_previous = true;
        }
      else
        {
          skipped++;
        }
    }

  if (ferror(stream))
    {
      ret = -EIO;
    }

  if (fclose(stream) != 0 && ret == OK)
    {
      ret = -errno;
    }

  if (ret == OK && have_previous)
    {
      if (previous_id == UINT32_MAX)
        {
          ret = -EOVERFLOW;
        }
      else
        {
          storage->next_id = previous_id + 1;
        }
    }

  syslog(LOG_INFO,
         "VelaPoka storage: history loaded=%u duplicates=%u skipped=%u "
         "next=%" PRIu32 "\n",
         loaded, duplicates, skipped, storage->next_id);
  return ret;
}

static int velapoka_storage_write_config(unsigned int threshold_percent)
{
  char temporary[VELAPOKA_STORAGE_PATH_SIZE];
  char destination[VELAPOKA_STORAGE_PATH_SIZE];
  char json[256];
  int length;
  int fd;
  int ret;

  ret = velapoka_path(temporary, sizeof(temporary),
                      VELAPOKA_STORAGE_CONFIG_TMP);
  if (ret < 0)
    {
      return ret;
    }

  ret = velapoka_path(destination, sizeof(destination),
                      VELAPOKA_STORAGE_CONFIG);
  if (ret < 0)
    {
      return ret;
    }

  length = snprintf(json, sizeof(json),
                    "{\n  \"version\": %u,\n"
                    "  \"width\": %u,\n  \"height\": %u,\n"
                    "  \"roi\": [%u, %u, %u, %u],\n"
                    "  \"threshold_percent\": %u\n}\n",
                    VELAPOKA_STORAGE_VERSION, VELAPOKA_INSPECT_WIDTH,
                    VELAPOKA_INSPECT_HEIGHT, VELAPOKA_ROI_X,
                    VELAPOKA_ROI_Y, VELAPOKA_ROI_WIDTH,
                    VELAPOKA_ROI_HEIGHT, threshold_percent);
  if (length <= 0 || (size_t)length >= sizeof(json))
    {
      return -EOVERFLOW;
    }

  fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (fd < 0)
    {
      return -errno;
    }

  ret = velapoka_write_all(fd, (FAR const uint8_t *)json, length);
  ret = velapoka_sync_close(fd, ret);
  if (ret < 0)
    {
      unlink(temporary);
      return ret;
    }

  return velapoka_replace(temporary, destination);
}

static int velapoka_storage_read_config(
  FAR unsigned int *threshold_percent)
{
  char path[VELAPOKA_STORAGE_PATH_SIZE];
  char line[128];
  FILE *stream;
  int ret;

  ret = velapoka_path(path, sizeof(path), VELAPOKA_STORAGE_CONFIG);
  if (ret < 0)
    {
      return ret;
    }

  stream = fopen(path, "r");
  if (stream == NULL)
    {
      return -errno;
    }

  ret = -EBADMSG;
  while (fgets(line, sizeof(line), stream) != NULL)
    {
      FAR char *field = strstr(line, "\"threshold_percent\"");
      unsigned int value;

      if (field != NULL &&
          sscanf(field, "\"threshold_percent\" : %u", &value) == 1)
        {
          if (value <= 100)
            {
              *threshold_percent = value;
              ret = OK;
            }
          else
            {
              ret = -ERANGE;
            }

          break;
        }
    }

  fclose(stream);
  return ret;
}

static int velapoka_storage_write_reference(
  FAR const uint8_t *reference, unsigned int threshold_percent)
{
  uint8_t header[VELAPOKA_STORAGE_HEADER_SIZE];
  char temporary[VELAPOKA_STORAGE_PATH_SIZE];
  char destination[VELAPOKA_STORAGE_PATH_SIZE];
  int fd;
  int ret;

  memset(header, 0, sizeof(header));
  velapoka_put_le32(&header[0], VELAPOKA_STORAGE_MAGIC);
  velapoka_put_le16(&header[4], VELAPOKA_STORAGE_VERSION);
  velapoka_put_le16(&header[6], VELAPOKA_STORAGE_HEADER_SIZE);
  velapoka_put_le16(&header[8], VELAPOKA_INSPECT_WIDTH);
  velapoka_put_le16(&header[10], VELAPOKA_INSPECT_HEIGHT);
  velapoka_put_le16(&header[12], VELAPOKA_ROI_X);
  velapoka_put_le16(&header[14], VELAPOKA_ROI_Y);
  velapoka_put_le16(&header[16], VELAPOKA_ROI_WIDTH);
  velapoka_put_le16(&header[18], VELAPOKA_ROI_HEIGHT);
  velapoka_put_le16(&header[20], threshold_percent);
  velapoka_put_le32(&header[24], VELAPOKA_INSPECT_SIZE);
  velapoka_put_le32(&header[28],
                    velapoka_crc32(reference, VELAPOKA_INSPECT_SIZE));

  ret = velapoka_path(temporary, sizeof(temporary),
                      VELAPOKA_STORAGE_TEMPLATE_TMP);
  if (ret < 0)
    {
      return ret;
    }

  ret = velapoka_path(destination, sizeof(destination),
                      VELAPOKA_STORAGE_TEMPLATE);
  if (ret < 0)
    {
      return ret;
    }

  fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (fd < 0)
    {
      return -errno;
    }

  ret = velapoka_write_all(fd, header, sizeof(header));
  if (ret == OK)
    {
      ret = velapoka_write_all(fd, reference, VELAPOKA_INSPECT_SIZE);
    }

  ret = velapoka_sync_close(fd, ret);
  if (ret < 0)
    {
      unlink(temporary);
      return ret;
    }

  ret = velapoka_replace(temporary, destination);
  if (ret == OK)
    {
      ret = velapoka_storage_write_config(threshold_percent);
    }

  return ret;
}

static int velapoka_storage_write_bmp(uint32_t id,
                                      FAR const uint8_t *gray,
                                      FAR char *relative,
                                      size_t relative_size)
{
  uint8_t header[VELAPOKA_BMP_HEADER_SIZE];
  char path[VELAPOKA_STORAGE_PATH_SIZE];
  uint32_t image_size = VELAPOKA_INSPECT_SIZE;
  uint32_t file_size = VELAPOKA_BMP_HEADER_SIZE + image_size;
  unsigned int i;
  int length;
  int fd;
  int ret;

  length = snprintf(relative, relative_size, "%s/fail_%08" PRIu32 ".bmp",
                    VELAPOKA_STORAGE_FAIL_DIR, id);
  if (length <= 0 || (size_t)length >= relative_size)
    {
      return -ENAMETOOLONG;
    }

  ret = velapoka_path(path, sizeof(path), relative);
  if (ret < 0)
    {
      return ret;
    }

  memset(header, 0, sizeof(header));
  header[0] = 'B';
  header[1] = 'M';
  velapoka_put_le32(&header[2], file_size);
  velapoka_put_le32(&header[10], VELAPOKA_BMP_HEADER_SIZE);
  velapoka_put_le32(&header[14], 40);
  velapoka_put_le32(&header[18], VELAPOKA_INSPECT_WIDTH);
  velapoka_put_le32(&header[22], VELAPOKA_INSPECT_HEIGHT);
  velapoka_put_le16(&header[26], 1);
  velapoka_put_le16(&header[28], 8);
  velapoka_put_le32(&header[34], image_size);
  velapoka_put_le32(&header[46], 256);
  for (i = 0; i < 256; i++)
    {
      header[54 + i * 4] = i;
      header[55 + i * 4] = i;
      header[56 + i * 4] = i;
    }

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (fd < 0)
    {
      return -errno;
    }

  ret = velapoka_write_all(fd, header, sizeof(header));
  for (i = 0; ret == OK && i < VELAPOKA_INSPECT_HEIGHT; i++)
    {
      FAR const uint8_t *row =
        gray + (VELAPOKA_INSPECT_HEIGHT - 1 - i) *
               VELAPOKA_INSPECT_WIDTH;
      ret = velapoka_write_all(fd, row, VELAPOKA_INSPECT_WIDTH);
    }

  return velapoka_sync_close(fd, ret);
}

static int velapoka_json_append(FAR char *json, size_t size,
                                FAR size_t *used, FAR const char *format,
                                ...)
{
  va_list args;
  int length;

  va_start(args, format);
  length = vsnprintf(json + *used, size - *used, format, args);
  va_end(args);
  if (length < 0 || (size_t)length >= size - *used)
    {
      return -EOVERFLOW;
    }

  *used += length;
  return OK;
}

static int velapoka_storage_write_result(
  uint32_t id, FAR const struct velapoka_result_s *result,
  FAR const uint8_t *gray, bool has_image,
  unsigned int threshold_percent)
{
  char path[VELAPOKA_STORAGE_PATH_SIZE];
  char image[48] = "";
  char json[VELAPOKA_STORAGE_JSON_SIZE];
  size_t used = 0;
  unsigned int i;
  int fd;
  int ret;

  if (has_image)
    {
      ret = velapoka_storage_write_bmp(id, gray, image, sizeof(image));
      if (ret < 0)
        {
          return ret;
        }
    }

  ret = velapoka_storage_write_config(threshold_percent);
  if (ret < 0)
    {
      return ret;
    }

  ret = velapoka_json_append(
    json, sizeof(json), &used,
    "{\"id\":%" PRIu32 ",\"frame\":%" PRIu32
    ",\"result\":\"%s\",\"score_tenths\":%u"
    ",\"difference_tenths\":%u,\"elapsed_ms\":%" PRIu32
    ",\"threshold_percent\":%u,\"image\":\"%s\",\"boxes\":[",
    id, result->sequence, result->pass ? "PASS" : "FAIL",
    result->score_tenths, result->difference_tenths,
    result->elapsed_ms, threshold_percent, image);
  for (i = 0; ret == OK && i < result->box_count; i++)
    {
      FAR const struct velapoka_box_s *box = &result->boxes[i];

      ret = velapoka_json_append(
        json, sizeof(json), &used,
        "%s{\"x\":%u,\"y\":%u,\"width\":%u,\"height\":%u}",
        i == 0 ? "" : ",", box->x, box->y, box->width, box->height);
    }

  if (ret == OK)
    {
      ret = velapoka_json_append(json, sizeof(json), &used, "]}\n");
    }

  if (ret < 0)
    {
      return ret;
    }

  ret = velapoka_path(path, sizeof(path), VELAPOKA_STORAGE_RESULTS);
  if (ret < 0)
    {
      return ret;
    }

  fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0664);
  if (fd < 0)
    {
      return -errno;
    }

  ret = velapoka_write_all(fd, (FAR const uint8_t *)json, used);
  return velapoka_sync_close(fd, ret);
}

static void velapoka_storage_set_error(
  FAR struct velapoka_storage_s *storage, int ret)
{
  pthread_mutex_lock(&storage->lock);
  storage->last_error = ret;
  storage->online = false;
  pthread_mutex_unlock(&storage->lock);
  syslog(LOG_ERR, "VelaPoka storage: I/O failed: %d\n", ret);
}

static FAR void *velapoka_storage_thread(FAR void *arg)
{
  FAR struct velapoka_storage_s *storage = arg;

  for (; ; )
    {
      FAR struct velapoka_storage_request_s *request = NULL;
      unsigned int threshold = 0;
      bool save_reference = false;
      bool save_result = false;
      int ret;

      pthread_mutex_lock(&storage->lock);
      while (!storage->reference_pending && storage->request_count == 0 &&
             !storage->stop)
        {
          pthread_cond_wait(&storage->cond, &storage->lock);
        }

      if (storage->stop && !storage->reference_pending &&
          storage->request_count == 0)
        {
          pthread_mutex_unlock(&storage->lock);
          break;
        }

      if (storage->reference_pending)
        {
          storage->reference_pending = false;
          storage->reference_busy = true;
          threshold = storage->reference_threshold;
          save_reference = true;
        }
      else if (storage->request_count > 0)
        {
          request = &storage->requests[storage->request_head];
          save_result = true;
        }

      pthread_mutex_unlock(&storage->lock);

      pthread_mutex_lock(&storage->io_lock);
      if (save_reference)
        {
          ret = velapoka_storage_write_reference(storage->reference,
                                                 threshold);
        }
      else if (save_result)
        {
          ret = velapoka_storage_write_result(
            request->id, &request->result, request->image,
            request->has_image, request->threshold);
        }
      else
        {
          ret = OK;
        }

      pthread_mutex_unlock(&storage->io_lock);

      if (ret < 0)
        {
          velapoka_storage_set_error(storage, ret);
        }

      pthread_mutex_lock(&storage->lock);
      if (save_reference)
        {
          storage->reference_busy = false;
        }

      if (save_result)
        {
          storage->request_head =
            (storage->request_head + 1) % VELAPOKA_STORAGE_QUEUE_DEPTH;
          storage->request_count--;
        }

      pthread_mutex_unlock(&storage->lock);
    }

  return NULL;
}

static int velapoka_storage_cleanup(
  FAR struct velapoka_storage_s *storage)
{
  unsigned int i;
  int ret = OK;

  for (i = 0; i < VELAPOKA_STORAGE_QUEUE_DEPTH; i++)
    {
      free(storage->requests[i].image);
    }

  free(storage->reference);
  if (storage->mounted_here &&
      umount(CONFIG_EXAMPLES_VELAPOKA_STORAGE_MOUNTPOINT) < 0)
    {
      ret = -errno;
      syslog(LOG_WARNING, "VelaPoka storage: umount %s failed: %d\n",
             CONFIG_EXAMPLES_VELAPOKA_STORAGE_MOUNTPOINT, errno);
    }
  else if (storage->mounted_here)
    {
      syslog(LOG_INFO, "VelaPoka storage: unmounted %s\n",
             CONFIG_EXAMPLES_VELAPOKA_STORAGE_MOUNTPOINT);
    }

  pthread_cond_destroy(&storage->cond);
  pthread_mutex_destroy(&storage->io_lock);
  pthread_mutex_destroy(&storage->lock);
  free(storage);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velapoka_storage_start(FAR struct velapoka_storage_s **storage_out)
{
  FAR struct velapoka_storage_s *storage;
  struct sched_param param;
  pthread_attr_t attr;
  int ret;

  if (storage_out == NULL)
    {
      return -EINVAL;
    }

  *storage_out = NULL;
  storage = calloc(1, sizeof(*storage));
  if (storage == NULL)
    {
      return -ENOMEM;
    }

  storage->next_id = 1;
  ret = pthread_mutex_init(&storage->lock, NULL);
  if (ret != 0)
    {
      free(storage);
      return -ret;
    }

  ret = pthread_mutex_init(&storage->io_lock, NULL);
  if (ret != 0)
    {
      pthread_mutex_destroy(&storage->lock);
      free(storage);
      return -ret;
    }

  ret = pthread_cond_init(&storage->cond, NULL);
  if (ret != 0)
    {
      pthread_mutex_destroy(&storage->io_lock);
      pthread_mutex_destroy(&storage->lock);
      free(storage);
      return -ret;
    }

  storage->reference = malloc(VELAPOKA_INSPECT_SIZE);
  for (unsigned int i = 0; i < VELAPOKA_STORAGE_QUEUE_DEPTH; i++)
    {
      storage->requests[i].image = malloc(VELAPOKA_INSPECT_SIZE);
    }

  if (storage->reference == NULL)
    {
      velapoka_storage_cleanup(storage);
      return -ENOMEM;
    }

  for (unsigned int i = 0; i < VELAPOKA_STORAGE_QUEUE_DEPTH; i++)
    {
      if (storage->requests[i].image == NULL)
        {
          velapoka_storage_cleanup(storage);
          return -ENOMEM;
        }
    }

  ret = velapoka_storage_mount(&storage->mounted_here);
  if (ret < 0)
    {
      velapoka_storage_cleanup(storage);
      return ret;
    }

  ret = velapoka_storage_load_history(storage);
  if (ret < 0)
    {
      syslog(LOG_ERR, "VelaPoka storage: load history failed: %d\n", ret);
      velapoka_storage_cleanup(storage);
      return ret;
    }

  storage->online = true;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr,
                            CONFIG_EXAMPLES_VELAPOKA_STORAGE_STACKSIZE);
  memset(&param, 0, sizeof(param));
  param.sched_priority = CONFIG_EXAMPLES_VELAPOKA_STORAGE_PRIORITY;
  pthread_attr_setschedparam(&attr, &param);
  ret = pthread_create(&storage->thread, &attr,
                       velapoka_storage_thread, storage);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      syslog(LOG_ERR, "VelaPoka storage: pthread_create failed: %d\n",
             ret);
      velapoka_storage_cleanup(storage);
      return -ret;
    }

  storage->thread_started = true;
  *storage_out = storage;
  syslog(LOG_INFO, "VelaPoka storage: mounted %s at %s\n",
         CONFIG_EXAMPLES_VELAPOKA_STORAGE_DEVPATH,
         CONFIG_EXAMPLES_VELAPOKA_STORAGE_MOUNTPOINT);
  return OK;
}

int velapoka_storage_stop(FAR struct velapoka_storage_s *storage)
{
  int cleanup_ret;
  int ret;

  if (storage == NULL)
    {
      return OK;
    }

  pthread_mutex_lock(&storage->lock);
  storage->stop = true;
  pthread_cond_signal(&storage->cond);
  pthread_mutex_unlock(&storage->lock);

  if (storage->thread_started)
    {
      pthread_join(storage->thread, NULL);
    }

  pthread_mutex_lock(&storage->lock);
  ret = storage->last_error;
  pthread_mutex_unlock(&storage->lock);

  cleanup_ret = velapoka_storage_cleanup(storage);
  return ret < 0 ? ret : cleanup_ret;
}

bool velapoka_storage_online(FAR struct velapoka_storage_s *storage)
{
  bool online;

  if (storage == NULL)
    {
      return false;
    }

  pthread_mutex_lock(&storage->lock);
  online = storage->online;
  pthread_mutex_unlock(&storage->lock);
  return online;
}

int velapoka_storage_last_error(FAR struct velapoka_storage_s *storage)
{
  int error;

  if (storage == NULL)
    {
      return -ENODEV;
    }

  pthread_mutex_lock(&storage->lock);
  error = storage->last_error;
  pthread_mutex_unlock(&storage->lock);
  return error;
}

int velapoka_storage_load_reference(
  FAR struct velapoka_storage_s *storage, FAR uint8_t *reference,
  size_t size, FAR unsigned int *threshold_percent)
{
  uint8_t header[VELAPOKA_STORAGE_HEADER_SIZE];
  char path[VELAPOKA_STORAGE_PATH_SIZE];
  int fd;
  int ret;

  if (storage == NULL || reference == NULL ||
      size != VELAPOKA_INSPECT_SIZE || threshold_percent == NULL)
    {
      return -EINVAL;
    }

  ret = velapoka_path(path, sizeof(path), VELAPOKA_STORAGE_TEMPLATE);
  if (ret < 0)
    {
      return ret;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  ret = velapoka_read_all(fd, header, sizeof(header));
  if (ret == OK)
    {
      ret = velapoka_read_all(fd, reference, size);
    }

  close(fd);
  if (ret < 0)
    {
      return ret;
    }

  if (velapoka_get_le32(&header[0]) != VELAPOKA_STORAGE_MAGIC ||
      velapoka_get_le16(&header[4]) != VELAPOKA_STORAGE_VERSION ||
      velapoka_get_le16(&header[6]) != VELAPOKA_STORAGE_HEADER_SIZE ||
      velapoka_get_le16(&header[8]) != VELAPOKA_INSPECT_WIDTH ||
      velapoka_get_le16(&header[10]) != VELAPOKA_INSPECT_HEIGHT ||
      velapoka_get_le16(&header[12]) != VELAPOKA_ROI_X ||
      velapoka_get_le16(&header[14]) != VELAPOKA_ROI_Y ||
      velapoka_get_le16(&header[16]) != VELAPOKA_ROI_WIDTH ||
      velapoka_get_le16(&header[18]) != VELAPOKA_ROI_HEIGHT ||
      velapoka_get_le32(&header[24]) != VELAPOKA_INSPECT_SIZE ||
      velapoka_get_le32(&header[28]) != velapoka_crc32(reference, size))
    {
      return -EBADMSG;
    }

  *threshold_percent = velapoka_get_le16(&header[20]);
  if (*threshold_percent > 100)
    {
      return -ERANGE;
    }

  ret = velapoka_storage_read_config(threshold_percent);
  if (ret < 0 && ret != -ENOENT && ret != -EBADMSG)
    {
      return ret;
    }

  return OK;
}

int velapoka_storage_save_reference(
  FAR struct velapoka_storage_s *storage, FAR const uint8_t *reference,
  size_t size, unsigned int threshold_percent)
{
  if (storage == NULL || reference == NULL ||
      size != VELAPOKA_INSPECT_SIZE || threshold_percent > 100)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&storage->lock);
  if (!storage->online)
    {
      int ret = storage->last_error != 0 ? storage->last_error : -ENODEV;
      pthread_mutex_unlock(&storage->lock);
      return ret;
    }

  if (storage->reference_pending || storage->reference_busy)
    {
      pthread_mutex_unlock(&storage->lock);
      return -EBUSY;
    }

  memcpy(storage->reference, reference, size);
  storage->reference_threshold = threshold_percent;
  storage->reference_pending = true;
  pthread_cond_signal(&storage->cond);
  pthread_mutex_unlock(&storage->lock);
  return OK;
}

int velapoka_storage_save_result(
  FAR struct velapoka_storage_s *storage,
  FAR const struct velapoka_result_s *result,
  FAR const uint8_t *gray, size_t size, unsigned int threshold_percent,
  FAR uint32_t *record_id)
{
  uint32_t id;
  FAR struct velapoka_storage_request_s *request;

  if (storage == NULL || result == NULL || record_id == NULL ||
      threshold_percent > 100 ||
      (!result->pass && (gray == NULL || size != VELAPOKA_INSPECT_SIZE)))
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&storage->lock);
  if (!storage->online)
    {
      int ret = storage->last_error != 0 ? storage->last_error : -ENODEV;
      pthread_mutex_unlock(&storage->lock);
      return ret;
    }

  if (storage->request_count >= VELAPOKA_STORAGE_QUEUE_DEPTH)
    {
      pthread_mutex_unlock(&storage->lock);
      return -EBUSY;
    }

  id = storage->next_id++;
  request = &storage->requests[storage->request_tail];
  request->result = *result;
  request->id = id;
  request->threshold = threshold_percent;
  request->has_image = !result->pass;
  if (!result->pass)
    {
      memcpy(request->image, gray, size);
    }

  velapoka_history_push(storage, id, result->pass,
                        result->score_tenths);
  storage->request_tail =
    (storage->request_tail + 1) % VELAPOKA_STORAGE_QUEUE_DEPTH;
  storage->request_count++;
  pthread_cond_signal(&storage->cond);
  pthread_mutex_unlock(&storage->lock);
  *record_id = id;
  return OK;
}

int velapoka_storage_read_export(
  FAR struct velapoka_storage_s *storage, FAR const char *relative_path,
  FAR uint8_t **data, FAR size_t *size)
{
  struct stat file_status;
  FAR uint8_t *snapshot;
  char path[VELAPOKA_STORAGE_PATH_SIZE];
  int fd;
  int ret;

  if (storage == NULL || relative_path == NULL || data == NULL ||
      size == NULL || !velapoka_export_path_valid(relative_path))
    {
      return -EINVAL;
    }

  *data = NULL;
  *size = 0;
  ret = velapoka_path(path, sizeof(path), relative_path);
  if (ret < 0)
    {
      return ret;
    }

  pthread_mutex_lock(&storage->io_lock);
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      ret = -errno;
      goto out_unlock;
    }

  if (fstat(fd, &file_status) < 0)
    {
      ret = -errno;
      close(fd);
      goto out_unlock;
    }

  if (file_status.st_size < 0 ||
      file_status.st_size > VELAPOKA_STORAGE_EXPORT_MAX)
    {
      ret = -EFBIG;
      close(fd);
      goto out_unlock;
    }

  snapshot = malloc(file_status.st_size > 0 ? file_status.st_size : 1);
  if (snapshot == NULL)
    {
      ret = -ENOMEM;
      close(fd);
      goto out_unlock;
    }

  ret = file_status.st_size > 0 ?
        velapoka_read_all(fd, snapshot, file_status.st_size) : OK;
  close(fd);
  if (ret < 0)
    {
      free(snapshot);
      goto out_unlock;
    }

  *data = snapshot;
  *size = file_status.st_size;

out_unlock:
  pthread_mutex_unlock(&storage->io_lock);
  return ret;
}

unsigned int velapoka_storage_get_history(
  FAR struct velapoka_storage_s *storage,
  FAR struct velapoka_history_s *history, unsigned int capacity)
{
  unsigned int count;

  if (storage == NULL || history == NULL || capacity == 0)
    {
      return 0;
    }

  pthread_mutex_lock(&storage->lock);
  count = storage->history_count < capacity ? storage->history_count :
                                              capacity;
  memcpy(history, storage->history, count * sizeof(*history));
  pthread_mutex_unlock(&storage->lock);
  return count;
}
