/****************************************************************************
 * apps/app/velapoka/velapoka_camera.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/videoio.h>
#include <unistd.h>

#include "velapoka_camera.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VELAPOKA_CAMERA_WIDTH          1280
#define VELAPOKA_CAMERA_HEIGHT         720
#define VELAPOKA_CAMERA_FPS            30
#define VELAPOKA_CAMERA_RAW10_SIZE \
  (VELAPOKA_CAMERA_WIDTH * VELAPOKA_CAMERA_HEIGHT * 5 / 4)
#define VELAPOKA_CAMERA_RAW_BUFFERS     2
#define VELAPOKA_CAMERA_PREVIEW_BUFFERS 2
#define VELAPOKA_CAMERA_BUFFER_ALIGN    64
#define VELAPOKA_CAMERA_POLL_TIMEOUT_MS 1000
#define VELAPOKA_CAMERA_MAX_TIMEOUTS    5

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum velapoka_preview_state_e
{
  VELAPOKA_PREVIEW_FREE = 0,
  VELAPOKA_PREVIEW_WRITING,
  VELAPOKA_PREVIEW_READY,
  VELAPOKA_PREVIEW_READING
};

struct velapoka_raw_buffer_s
{
  FAR uint8_t *data;
};

struct velapoka_preview_buffer_s
{
  FAR uint8_t *data;
  enum velapoka_preview_state_e state;
  uint32_t sequence;
};

struct velapoka_camera_s
{
  struct velapoka_raw_buffer_s raw[VELAPOKA_CAMERA_RAW_BUFFERS];
  struct velapoka_preview_buffer_s preview[VELAPOKA_CAMERA_PREVIEW_BUFFERS];
  pthread_mutex_t lock;
  pthread_t thread;
  unsigned int raw_count;
  int latest;
  int last_error;
  int fd;
  bool stop;
  bool streaming;
  bool thread_started;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool velapoka_camera_should_stop(FAR struct velapoka_camera_s *camera)
{
  bool stop;

  pthread_mutex_lock(&camera->lock);
  stop = camera->stop;
  pthread_mutex_unlock(&camera->lock);
  return stop;
}

static int velapoka_camera_reserve_preview(
  FAR struct velapoka_camera_s *camera)
{
  unsigned int i;
  int index = -1;

  pthread_mutex_lock(&camera->lock);
  for (i = 0; i < VELAPOKA_CAMERA_PREVIEW_BUFFERS; i++)
    {
      if (camera->preview[i].state == VELAPOKA_PREVIEW_FREE)
        {
          index = (int)i;
          break;
        }
    }

  if (index < 0 && camera->latest >= 0 &&
      camera->preview[camera->latest].state == VELAPOKA_PREVIEW_READY)
    {
      index = camera->latest;
      camera->latest = -1;
    }

  if (index >= 0)
    {
      camera->preview[index].state = VELAPOKA_PREVIEW_WRITING;
    }

  pthread_mutex_unlock(&camera->lock);
  return index;
}

static void velapoka_camera_publish_preview(
  FAR struct velapoka_camera_s *camera, int index, uint32_t sequence)
{
  pthread_mutex_lock(&camera->lock);
  if (camera->latest >= 0 && camera->latest != index &&
      camera->preview[camera->latest].state == VELAPOKA_PREVIEW_READY)
    {
      camera->preview[camera->latest].state = VELAPOKA_PREVIEW_FREE;
    }

  camera->preview[index].sequence = sequence;
  camera->preview[index].state = VELAPOKA_PREVIEW_READY;
  camera->latest = index;
  pthread_mutex_unlock(&camera->lock);
}

static void velapoka_camera_convert(FAR const uint8_t *raw,
                                    FAR uint8_t *preview)
{
  unsigned int x;
  unsigned int y;

  for (y = 0; y < VELAPOKA_CAMERA_PREVIEW_HEIGHT; y++)
    {
      unsigned int sy = y * VELAPOKA_CAMERA_HEIGHT /
                        VELAPOKA_CAMERA_PREVIEW_HEIGHT;
      FAR const uint8_t *src = raw + sy * VELAPOKA_CAMERA_WIDTH * 5 / 4;
      FAR uint8_t *dst = preview + y * VELAPOKA_CAMERA_PREVIEW_WIDTH;

      for (x = 0; x < VELAPOKA_CAMERA_PREVIEW_WIDTH; x++)
        {
          unsigned int sx = x * VELAPOKA_CAMERA_WIDTH /
                            VELAPOKA_CAMERA_PREVIEW_WIDTH;
          unsigned int value = src[(sx / 4) * 5 + sx % 4] *
                               CONFIG_EXAMPLES_VELAPOKA_PREVIEW_GAIN;

          dst[x] = value > UINT8_MAX ? UINT8_MAX : (uint8_t)value;
        }
    }
}

static int velapoka_camera_requeue(FAR struct velapoka_camera_s *camera,
                                   FAR struct v4l2_buffer *buffer)
{
  if (ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)buffer) < 0)
    {
      return -errno;
    }

  return OK;
}

static FAR void *velapoka_camera_thread(FAR void *arg)
{
  FAR struct velapoka_camera_s *camera = arg;
  unsigned int timeouts = 0;
  int error = 0;

  while (!velapoka_camera_should_stop(camera))
    {
      struct v4l2_buffer buffer;
      struct pollfd pfd;
      int preview_index = -1;
      int ret;

      memset(&pfd, 0, sizeof(pfd));
      pfd.fd = camera->fd;
      pfd.events = POLLIN;
      ret = poll(&pfd, 1, VELAPOKA_CAMERA_POLL_TIMEOUT_MS);
      if (ret == 0)
        {
          if (++timeouts >= VELAPOKA_CAMERA_MAX_TIMEOUTS)
            {
              error = -ETIMEDOUT;
              break;
            }

          continue;
        }

      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          error = -errno;
          break;
        }

      timeouts = 0;
      if ((pfd.revents & POLLIN) == 0)
        {
          error = -EIO;
          break;
        }

      memset(&buffer, 0, sizeof(buffer));
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_USERPTR;
      if (ioctl(camera->fd, VIDIOC_DQBUF, (uintptr_t)&buffer) < 0)
        {
          error = -errno;
          break;
        }

      if (buffer.index >= camera->raw_count ||
          buffer.m.userptr !=
            (uintptr_t)camera->raw[buffer.index].data ||
          buffer.bytesused != VELAPOKA_CAMERA_RAW10_SIZE ||
          (buffer.flags & V4L2_BUF_FLAG_ERROR) != 0)
        {
          error = -EIO;
        }
      else
        {
          preview_index = velapoka_camera_reserve_preview(camera);
          if (preview_index >= 0)
            {
              velapoka_camera_convert(camera->raw[buffer.index].data,
                                      camera->preview[preview_index].data);
              velapoka_camera_publish_preview(camera, preview_index,
                                               buffer.sequence);
            }
        }

      ret = velapoka_camera_requeue(camera, &buffer);
      if (ret < 0)
        {
          error = ret;
        }

      if (error < 0)
        {
          break;
        }
    }

  pthread_mutex_lock(&camera->lock);
  camera->last_error = error;
  pthread_mutex_unlock(&camera->lock);

  if (error < 0)
    {
      fprintf(stderr, "velapoka: camera capture stopped: %d\n", error);
    }

  return NULL;
}

static void velapoka_camera_cleanup(FAR struct velapoka_camera_s *camera)
{
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  unsigned int i;

  if (camera->streaming)
    {
      ioctl(camera->fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
    }

  if (camera->fd >= 0)
    {
      close(camera->fd);
    }

  for (i = 0; i < VELAPOKA_CAMERA_RAW_BUFFERS; i++)
    {
      free(camera->raw[i].data);
    }

  for (i = 0; i < VELAPOKA_CAMERA_PREVIEW_BUFFERS; i++)
    {
      free(camera->preview[i].data);
    }

  pthread_mutex_destroy(&camera->lock);
  free(camera);
}

static int velapoka_camera_configure(FAR struct velapoka_camera_s *camera)
{
  struct v4l2_requestbuffers request;
  struct v4l2_streamparm parm;
  struct v4l2_format format;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  unsigned int i;

  camera->fd = open(CONFIG_EXAMPLES_VELAPOKA_CAMERA_DEVPATH, O_RDWR);
  if (camera->fd < 0)
    {
      return -errno;
    }

  memset(&format, 0, sizeof(format));
  format.type = type;
  format.fmt.pix.width = VELAPOKA_CAMERA_WIDTH;
  format.fmt.pix.height = VELAPOKA_CAMERA_HEIGHT;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_SBGGR10P;
  if (ioctl(camera->fd, VIDIOC_S_FMT, (uintptr_t)&format) < 0)
    {
      return -errno;
    }

  if (format.fmt.pix.width != VELAPOKA_CAMERA_WIDTH ||
      format.fmt.pix.height != VELAPOKA_CAMERA_HEIGHT ||
      format.fmt.pix.pixelformat != V4L2_PIX_FMT_SBGGR10P)
    {
      return -ENOTSUP;
    }

  memset(&parm, 0, sizeof(parm));
  parm.type = type;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = VELAPOKA_CAMERA_FPS;
  if (ioctl(camera->fd, VIDIOC_S_PARM, (uintptr_t)&parm) < 0)
    {
      return -errno;
    }

  memset(&request, 0, sizeof(request));
  request.type = type;
  request.memory = V4L2_MEMORY_USERPTR;
  request.count = VELAPOKA_CAMERA_RAW_BUFFERS;
  request.mode = V4L2_BUF_MODE_FIFO;
  if (ioctl(camera->fd, VIDIOC_REQBUFS, (uintptr_t)&request) < 0)
    {
      return -errno;
    }

  if (request.count < 1 || request.count > VELAPOKA_CAMERA_RAW_BUFFERS)
    {
      return -ENOBUFS;
    }

  camera->raw_count = request.count;
  for (i = 0; i < camera->raw_count; i++)
    {
      struct v4l2_buffer buffer;

      camera->raw[i].data =
        memalign(VELAPOKA_CAMERA_BUFFER_ALIGN, VELAPOKA_CAMERA_RAW10_SIZE);
      if (camera->raw[i].data == NULL)
        {
          return -ENOMEM;
        }

      memset(&buffer, 0, sizeof(buffer));
      buffer.type = type;
      buffer.memory = V4L2_MEMORY_USERPTR;
      buffer.index = i;
      buffer.m.userptr = (uintptr_t)camera->raw[i].data;
      buffer.length = VELAPOKA_CAMERA_RAW10_SIZE;
      if (ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0)
        {
          return -errno;
        }
    }

  for (i = 0; i < VELAPOKA_CAMERA_PREVIEW_BUFFERS; i++)
    {
      camera->preview[i].data =
        memalign(VELAPOKA_CAMERA_BUFFER_ALIGN,
                 VELAPOKA_CAMERA_PREVIEW_SIZE);
      if (camera->preview[i].data == NULL)
        {
          return -ENOMEM;
        }
    }

  if (ioctl(camera->fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      return -errno;
    }

  camera->streaming = true;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velapoka_camera_start(FAR struct velapoka_camera_s **camera_out)
{
  FAR struct velapoka_camera_s *camera;
  struct sched_param param;
  pthread_attr_t attr;
  int ret;

  if (camera_out == NULL)
    {
      return -EINVAL;
    }

  *camera_out = NULL;
  camera = calloc(1, sizeof(*camera));
  if (camera == NULL)
    {
      return -ENOMEM;
    }

  camera->fd = -1;
  camera->latest = -1;
  ret = pthread_mutex_init(&camera->lock, NULL);
  if (ret != 0)
    {
      free(camera);
      return -ret;
    }

  ret = velapoka_camera_configure(camera);
  if (ret < 0)
    {
      velapoka_camera_cleanup(camera);
      return ret;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr,
                            CONFIG_EXAMPLES_VELAPOKA_CAMERA_STACKSIZE);
  memset(&param, 0, sizeof(param));
  param.sched_priority = CONFIG_EXAMPLES_VELAPOKA_CAMERA_PRIORITY;
  pthread_attr_setschedparam(&attr, &param);
  ret = pthread_create(&camera->thread, &attr,
                       velapoka_camera_thread, camera);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      velapoka_camera_cleanup(camera);
      return -ret;
    }

  camera->thread_started = true;
  *camera_out = camera;
  return OK;
}

int velapoka_camera_acquire(FAR struct velapoka_camera_s *camera,
                            FAR struct velapoka_camera_frame_s *frame)
{
  int ret = 0;
  int index;

  if (camera == NULL || frame == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&camera->lock);
  index = camera->latest;
  if (index >= 0 && camera->preview[index].state == VELAPOKA_PREVIEW_READY)
    {
      camera->preview[index].state = VELAPOKA_PREVIEW_READING;
      camera->latest = -1;
      frame->data = camera->preview[index].data;
      frame->size = VELAPOKA_CAMERA_PREVIEW_SIZE;
      frame->sequence = camera->preview[index].sequence;
      frame->index = (uint8_t)index;
      ret = 1;
    }
  else if (camera->last_error < 0)
    {
      ret = camera->last_error;
    }

  pthread_mutex_unlock(&camera->lock);
  return ret;
}

void velapoka_camera_release(FAR struct velapoka_camera_s *camera,
                             FAR const struct velapoka_camera_frame_s *frame)
{
  if (camera == NULL || frame == NULL ||
      frame->index >= VELAPOKA_CAMERA_PREVIEW_BUFFERS)
    {
      return;
    }

  pthread_mutex_lock(&camera->lock);
  if (camera->preview[frame->index].state == VELAPOKA_PREVIEW_READING)
    {
      camera->preview[frame->index].state = VELAPOKA_PREVIEW_FREE;
    }

  pthread_mutex_unlock(&camera->lock);
}

void velapoka_camera_stop(FAR struct velapoka_camera_s *camera)
{
  if (camera == NULL)
    {
      return;
    }

  pthread_mutex_lock(&camera->lock);
  camera->stop = true;
  pthread_mutex_unlock(&camera->lock);

  if (camera->thread_started)
    {
      pthread_join(camera->thread, NULL);
    }

  velapoka_camera_cleanup(camera);
}
