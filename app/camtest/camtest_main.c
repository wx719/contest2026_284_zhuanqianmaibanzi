/****************************************************************************
 * apps/examples/camtest/camtest_main.c
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/videoio.h>
#include <unistd.h>

#include <nuttx/video/fb.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAMTEST_DEVICE          "/dev/video0"
#define CAMTEST_WIDTH           1280
#define CAMTEST_HEIGHT          720
#define CAMTEST_FPS             30
#define CAMTEST_RAW10_SIZE      (CAMTEST_WIDTH * CAMTEST_HEIGHT * 5 / 4)
#define CAMTEST_BUFFER_COUNT    2
#define CAMTEST_BUFFER_ALIGN    64
#define CAMTEST_DEFAULT_FRAMES  1
#define CAMTEST_DEFAULT_TIMEOUT 5000
#define CAMTEST_MAX_FRAMES      100
#define CAMTEST_FB_DEVICE       "/dev/fb0"
#define CAMTEST_PREVIEW_WIDTH   1024
#define CAMTEST_PREVIEW_HEIGHT  576
#define CAMTEST_PREVIEW_Y       12
#define CAMTEST_PREVIEW_GAIN    4

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct camtest_buffer_s
{
  FAR uint8_t *data;
};

struct camtest_fb_s
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  FAR uint8_t *memory;
  int fd;
  unsigned int draw_buffer;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void camtest_usage(FAR const char *progname)
{
  printf("Usage: %s [frames [timeout_ms]]\n", progname);
  printf("       %s preview [frames [timeout_ms]]\n", progname);
  printf("  frames: 1..%d (default %d)\n",
         CAMTEST_MAX_FRAMES, CAMTEST_DEFAULT_FRAMES);
  printf("  timeout_ms: per-frame timeout (default %d)\n",
         CAMTEST_DEFAULT_TIMEOUT);
}

static int camtest_fb_open(FAR struct camtest_fb_s *fb)
{
  memset(fb, 0, sizeof(*fb));
  fb->fd = open(CAMTEST_FB_DEVICE, O_RDWR);
  if (fb->fd < 0)
    {
      return -errno;
    }

  if (ioctl(fb->fd, FBIOGET_VIDEOINFO, (uintptr_t)&fb->vinfo) < 0 ||
      ioctl(fb->fd, FBIOGET_PLANEINFO, (uintptr_t)&fb->pinfo) < 0 ||
      fb->vinfo.xres != CAMTEST_PREVIEW_WIDTH ||
      fb->vinfo.yres != 600 || fb->pinfo.bpp != 16 ||
      fb->pinfo.yres_virtual < fb->vinfo.yres * 2)
    {
      close(fb->fd);
      fb->fd = -1;
      return -ENOTSUP;
    }

  fb->memory = mmap(NULL, fb->pinfo.fblen, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_FILE, fb->fd, 0);
  if (fb->memory == MAP_FAILED)
    {
      close(fb->fd);
      fb->fd = -1;
      return -errno;
    }

  fb->draw_buffer = 1;
  return OK;
}

static void camtest_fb_close(FAR struct camtest_fb_s *fb)
{
  if (fb->memory != NULL && fb->memory != MAP_FAILED)
    {
      munmap(fb->memory, fb->pinfo.fblen);
    }

  if (fb->fd >= 0)
    {
      close(fb->fd);
    }
}

static void camtest_render_preview(FAR struct camtest_fb_s *fb,
                                   FAR const uint8_t *raw)
{
  FAR uint8_t *frame;
  unsigned int x;
  unsigned int y;

  frame = fb->memory + fb->draw_buffer * fb->vinfo.yres * fb->pinfo.stride;
  memset(frame, 0, fb->vinfo.yres * fb->pinfo.stride);

  for (y = 0; y < CAMTEST_PREVIEW_HEIGHT; y++)
    {
      unsigned int sy = y * CAMTEST_HEIGHT / CAMTEST_PREVIEW_HEIGHT;
      FAR const uint8_t *src = raw + sy * CAMTEST_WIDTH * 5 / 4;
      FAR uint16_t *dst = (FAR uint16_t *)(frame +
        (y + CAMTEST_PREVIEW_Y) * fb->pinfo.stride);

      for (x = 0; x < CAMTEST_PREVIEW_WIDTH; x++)
        {
          unsigned int sx = x * CAMTEST_WIDTH / CAMTEST_PREVIEW_WIDTH;
          unsigned int value;
          uint8_t gray;

          value = src[(sx / 4) * 5 + sx % 4] * CAMTEST_PREVIEW_GAIN;
          gray = value > UINT8_MAX ? UINT8_MAX : (uint8_t)value;

          if (x == 0 || x == CAMTEST_PREVIEW_WIDTH - 1 || y == 0 ||
              y == CAMTEST_PREVIEW_HEIGHT - 1)
            {
              gray = UINT8_MAX;
            }

          dst[x] = ((uint16_t)(gray & 0xf8) << 8) |
                   ((uint16_t)(gray & 0xfc) << 3) | (gray >> 3);
        }
    }
}

static int camtest_commit_preview(FAR struct camtest_fb_s *fb)
{
  struct fb_planeinfo_s pan = fb->pinfo;
  struct pollfd pollfd;
  int ret;

  memset(&pollfd, 0, sizeof(pollfd));
  pollfd.fd = fb->fd;
  pollfd.events = POLLOUT;
  ret = poll(&pollfd, 1, 1000);
  if (ret <= 0 || (pollfd.revents & POLLOUT) == 0)
    {
      return ret == 0 ? -ETIMEDOUT : -EIO;
    }

  pan.xoffset = 0;
  pan.yoffset = fb->draw_buffer * fb->vinfo.yres;
  if (ioctl(fb->fd, FBIOPAN_DISPLAY, (uintptr_t)&pan) < 0)
    {
      return -errno;
    }

  fb->draw_buffer ^= 1;
  return OK;
}

static int camtest_parse_positive(FAR const char *text, int maximum,
                                  FAR int *value)
{
  FAR char *end;
  long parsed;

  errno = 0;
  parsed = strtol(text, &end, 10);
  if (errno != 0 || *text == '\0' || *end != '\0' || parsed < 1 ||
      parsed > maximum)
    {
      return -EINVAL;
    }

  *value = (int)parsed;
  return OK;
}

static uint32_t camtest_checksum(FAR const uint8_t *data, size_t size,
                                 FAR uint8_t *minimum,
                                 FAR uint8_t *maximum)
{
  uint32_t hash = UINT32_C(2166136261);
  uint8_t minval = UINT8_MAX;
  uint8_t maxval = 0;
  size_t i;

  for (i = 0; i < size; i++)
    {
      if (data[i] < minval)
        {
          minval = data[i];
        }

      if (data[i] > maxval)
        {
          maxval = data[i];
        }

      hash ^= data[i];
      hash *= UINT32_C(16777619);
    }

  *minimum = minval;
  *maximum = maxval;
  return hash;
}

static void camtest_dump_prefix(FAR const uint8_t *data, size_t size)
{
  size_t count = size < 16 ? size : 16;
  size_t i;

  printf(" prefix=");
  for (i = 0; i < count; i++)
    {
      printf("%02x", data[i]);
    }
}

static void camtest_free_buffers(FAR struct camtest_buffer_s *buffers,
                                 unsigned int count)
{
  unsigned int i;

  for (i = 0; i < count; i++)
    {
      free(buffers[i].data);
      buffers[i].data = NULL;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct camtest_buffer_s buffers[CAMTEST_BUFFER_COUNT];
  struct v4l2_requestbuffers request;
  struct v4l2_streamparm parm;
  struct v4l2_format format;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  unsigned int allocated = 0;
  unsigned int i;
  int timeout_ms = CAMTEST_DEFAULT_TIMEOUT;
  int frames = CAMTEST_DEFAULT_FRAMES;
  struct camtest_fb_s preview_fb;
  int argbase = 1;
  bool preview = false;
  bool preview_ready = false;
  bool streaming = false;
  int fd = -1;
  int ret = EXIT_FAILURE;

  preview_fb.fd = -1;
  preview_fb.memory = NULL;

  memset(buffers, 0, sizeof(buffers));
  memset(&request, 0, sizeof(request));
  memset(&parm, 0, sizeof(parm));
  memset(&format, 0, sizeof(format));

  if (argc > 1 && strcmp(argv[1], "preview") == 0)
    {
      preview = true;
      frames = CAMTEST_MAX_FRAMES;
      argbase = 2;
    }

  if (argc > argbase + 2 ||
      (argc > argbase &&
       camtest_parse_positive(argv[argbase], CAMTEST_MAX_FRAMES,
                                           &frames) < 0) ||
      (argc > argbase + 1 &&
       camtest_parse_positive(argv[argbase + 1], INT32_MAX,
                                           &timeout_ms) < 0))
    {
      camtest_usage(argv[0]);
      return EXIT_FAILURE;
    }

  if (preview)
    {
      ret = camtest_fb_open(&preview_fb);
      if (ret < 0)
        {
          printf("camtest: preview framebuffer unavailable: %d\n", -ret);
          return EXIT_FAILURE;
        }

      /* Keep DPI scanning the front buffer while CSI captures into its own
       * buffers and the CPU renders into the framebuffer back buffer.  CSI
       * and DSI own separate channels allocated by the shared DW-GDMA core.
       * Stopping and recreating the DSI channel here can starve the bridge
       * during resume; once the bridge underruns, the panel remains blue.
       * A single pan after STREAMOFF changes buffers at a frame boundary.
       */

      printf("camtest: preview uses continuous display with back-buffer "
             "rendering\n");
    }

  fd = open(CAMTEST_DEVICE, O_RDWR);
  if (fd < 0)
    {
      printf("camtest: open %s failed: %d\n", CAMTEST_DEVICE, errno);
      goto out;
    }

  printf("camtest: camera opened\n");

  format.type = type;
  format.fmt.pix.width = CAMTEST_WIDTH;
  format.fmt.pix.height = CAMTEST_HEIGHT;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_SBGGR10P;
  if (ioctl(fd, VIDIOC_S_FMT, (uintptr_t)&format) < 0)
    {
      printf("camtest: VIDIOC_S_FMT failed: %d\n", errno);
      goto out;
    }

  if (format.fmt.pix.width != CAMTEST_WIDTH ||
      format.fmt.pix.height != CAMTEST_HEIGHT ||
      format.fmt.pix.pixelformat != V4L2_PIX_FMT_SBGGR10P)
    {
      printf("camtest: driver selected unsupported format %lux%lu "
             "0x%08lx\n",
             (unsigned long)format.fmt.pix.width,
             (unsigned long)format.fmt.pix.height,
             (unsigned long)format.fmt.pix.pixelformat);
      goto out;
    }

  printf("camtest: format configured\n");

  parm.type = type;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = CAMTEST_FPS;
  if (ioctl(fd, VIDIOC_S_PARM, (uintptr_t)&parm) < 0)
    {
      printf("camtest: VIDIOC_S_PARM failed: %d\n", errno);
      goto out;
    }

  printf("camtest: frame interval configured\n");

  request.type = type;
  request.memory = V4L2_MEMORY_USERPTR;
  request.count = CAMTEST_BUFFER_COUNT;
  request.mode = V4L2_BUF_MODE_FIFO;
  if (ioctl(fd, VIDIOC_REQBUFS, (uintptr_t)&request) < 0)
    {
      printf("camtest: VIDIOC_REQBUFS failed: %d\n", errno);
      goto out;
    }

  printf("camtest: requested %lu capture buffers\n",
         (unsigned long)request.count);

  if (request.count < 1 || request.count > CAMTEST_BUFFER_COUNT)
    {
      printf("camtest: invalid buffer count %lu\n",
             (unsigned long)request.count);
      goto out;
    }

  for (i = 0; i < request.count; i++)
    {
      struct v4l2_buffer buffer;

      memset(&buffer, 0, sizeof(buffer));
      buffers[i].data =
        memalign(CAMTEST_BUFFER_ALIGN, CAMTEST_RAW10_SIZE);
      if (buffers[i].data == NULL)
        {
          printf("camtest: frame buffer allocation failed at %u\n",
                 i);
          goto out;
        }

      allocated = i + 1;
      printf("camtest: allocated buffer %u at %p\n", i,
             buffers[i].data);
      buffer.type = type;
      buffer.memory = V4L2_MEMORY_USERPTR;
      buffer.index = i;
      buffer.m.userptr = (uintptr_t)buffers[i].data;
      buffer.length = CAMTEST_RAW10_SIZE;
      if (ioctl(fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0)
        {
          printf("camtest: VIDIOC_QBUF[%u] failed: %d\n",
                 i, errno);
          goto out;
        }
    }

  printf("camtest: %ux%u BGGR10P, %u bytes, %lu buffer(s), %d fps\n",
         CAMTEST_WIDTH, CAMTEST_HEIGHT, CAMTEST_RAW10_SIZE,
         (unsigned long)request.count, CAMTEST_FPS);

  if (ioctl(fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      printf("camtest: VIDIOC_STREAMON failed: %d\n", errno);
      goto out;
    }

  streaming = true;
  for (i = 0; i < (unsigned int)frames; i++)
    {
      struct pollfd pollfd;
      struct v4l2_buffer buffer;
      uint32_t checksum;
      uint8_t minimum;
      uint8_t maximum;

      memset(&pollfd, 0, sizeof(pollfd));
      pollfd.fd = fd;
      pollfd.events = POLLIN;
      memset(&buffer, 0, sizeof(buffer));

      ret = poll(&pollfd, 1, timeout_ms);
      if (ret == 0)
        {
          printf("camtest: frame %u timed out after %d ms\n",
                 i, timeout_ms);
          ret = EXIT_FAILURE;
          goto out;
        }

      if (ret < 0)
        {
          printf("camtest: poll failed: %d\n", errno);
          ret = EXIT_FAILURE;
          goto out;
        }

      if ((pollfd.revents & POLLIN) == 0)
        {
          printf("camtest: poll returned events 0x%04lx\n",
                 (unsigned long)pollfd.revents);
          ret = EXIT_FAILURE;
          goto out;
        }

      buffer.type = type;
      buffer.memory = V4L2_MEMORY_USERPTR;
      if (ioctl(fd, VIDIOC_DQBUF, (uintptr_t)&buffer) < 0)
        {
          printf("camtest: VIDIOC_DQBUF failed: %d\n", errno);
          ret = EXIT_FAILURE;
          goto out;
        }

      if (buffer.index >= request.count ||
          buffer.m.userptr != (uintptr_t)buffers[buffer.index].data ||
          buffer.bytesused > CAMTEST_RAW10_SIZE)
        {
          printf("camtest: invalid completed buffer index=%lu size=%lu\n",
                 (unsigned long)buffer.index,
                 (unsigned long)buffer.bytesused);
          ret = EXIT_FAILURE;
          goto out;
        }

      checksum = camtest_checksum(buffers[buffer.index].data,
                                  buffer.bytesused, &minimum, &maximum);
      printf("camtest: frame %u sequence=%lu bytes=%lu fnv1a=%08lx "
             "range=%02x..%02x",
             i, (unsigned long)buffer.sequence,
             (unsigned long)buffer.bytesused, (unsigned long)checksum,
             minimum, maximum);
      camtest_dump_prefix(buffers[buffer.index].data, buffer.bytesused);
      printf("\n");

      if (buffer.bytesused != CAMTEST_RAW10_SIZE ||
          (buffer.flags & V4L2_BUF_FLAG_ERROR) != 0)
        {
          printf("camtest: invalid frame size or error flag 0x%08lx\n",
                 (unsigned long)buffer.flags);
          ret = EXIT_FAILURE;
          goto out;
        }

      if (preview)
        {
          camtest_render_preview(&preview_fb, buffers[buffer.index].data);
          preview_ready = true;
        }

      if (i + 1 < (unsigned int)frames &&
          ioctl(fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0)
        {
          printf("camtest: requeue failed: %d\n", errno);
          ret = EXIT_FAILURE;
          goto out;
        }
    }

  printf("camtest: PASS, captured %d frame(s)\n", frames);
  ret = EXIT_SUCCESS;

out:
  if (streaming)
    {
      if (ioctl(fd, VIDIOC_STREAMOFF, (uintptr_t)&type) < 0)
        {
          printf("camtest: VIDIOC_STREAMOFF failed: %d\n", errno);
          ret = EXIT_FAILURE;
        }
    }

  if (ret == EXIT_SUCCESS && preview_ready)
    {
      int preview_ret = camtest_commit_preview(&preview_fb);

      if (preview_ret < 0)
        {
          printf("camtest: framebuffer preview failed: %d\n",
                 -preview_ret);
          ret = EXIT_FAILURE;
        }
    }

  if (fd >= 0)
    {
      close(fd);
    }

  camtest_free_buffers(buffers, allocated);
  camtest_fb_close(&preview_fb);
  return ret;
}
