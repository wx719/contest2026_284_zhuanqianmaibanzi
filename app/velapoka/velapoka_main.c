/****************************************************************************
 * apps/app/velapoka/velapoka_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/video/fb.h>

#include <lvgl/lvgl.h>

#include "velapoka_camera.h"
#include "velapoka_ui.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
  FAR struct velapoka_camera_s *camera = NULL;
  bool touch_online;
  bool camera_online;
  int sync_fd = -1;
  int ret;

  if (lv_is_initialized())
    {
      fprintf(stderr, "velapoka: LVGL is already running\n");
      return -EBUSY;
    }

  lv_init();
  lv_nuttx_dsc_init(&info);
  info.fb_path = CONFIG_EXAMPLES_VELAPOKA_FB_DEVPATH;
  info.input_path = CONFIG_EXAMPLES_VELAPOKA_INPUT_DEVPATH;
  lv_nuttx_init(&info, &result);

  if (result.disp == NULL)
    {
      fprintf(stderr, "velapoka: cannot open %s\n", info.fb_path);
      lv_nuttx_deinit(&result);
      lv_deinit();
      return -ENODEV;
    }

  touch_online = result.indev != NULL;
  if (!touch_online)
    {
      fprintf(stderr, "velapoka: warning: cannot open %s\n",
              info.input_path);
    }

  ret = velapoka_camera_start(&camera);
  camera_online = ret == OK;
  if (!camera_online)
    {
      fprintf(stderr, "velapoka: warning: cannot start %s: %d\n",
              CONFIG_EXAMPLES_VELAPOKA_CAMERA_DEVPATH, ret);
    }

  lv_display_set_default(result.disp);
  sync_fd = open(info.fb_path, O_RDWR | O_CLOEXEC);
  if (sync_fd < 0)
    {
      fprintf(stderr, "velapoka: warning: cannot sync to %s: %d\n",
              info.fb_path, errno);
    }

  ret = velapoka_ui_create(touch_online, camera_online);
  if (ret < 0)
    {
      fprintf(stderr, "velapoka: UI creation failed: %d\n", ret);
      close(sync_fd);
      velapoka_camera_stop(camera);
      lv_nuttx_deinit(&result);
      lv_deinit();
      return ret;
    }

  printf("velapoka: live preview ready, fb=%s touch=%s camera=%s\n",
         info.fb_path, touch_online ? info.input_path : "offline",
         camera_online ? CONFIG_EXAMPLES_VELAPOKA_CAMERA_DEVPATH :
                         "offline");

  for (; ; )
    {
      uint32_t idle;

      if (camera != NULL)
        {
          struct velapoka_camera_frame_s frame;

          ret = velapoka_camera_acquire(camera, &frame);
          if (ret > 0)
            {
              ret = velapoka_ui_update_preview(frame.data, frame.size,
                                               frame.sequence);
              velapoka_camera_release(camera, &frame);
              if (ret < 0)
                {
                  fprintf(stderr, "velapoka: preview update failed: %d\n",
                          ret);
                }
            }
          else if (ret < 0)
            {
              fprintf(stderr, "velapoka: camera offline: %d\n", ret);
              velapoka_ui_set_camera_online(false);
              velapoka_camera_stop(camera);
              camera = NULL;
            }
        }

      if (sync_fd >= 0)
        {
          do
            {
              ret = ioctl(sync_fd, FBIO_WAITFORVSYNC, 0);
            }
          while (ret < 0 && errno == EINTR);

          if (ret < 0)
            {
              fprintf(stderr, "velapoka: VSYNC wait failed: %d\n", errno);
              close(sync_fd);
              sync_fd = -1;
            }
        }

      idle = lv_timer_handler();

      if (sync_fd >= 0)
        {
          idle = 1;
        }
      else if (idle < 1)
        {
          idle = 1;
        }
      else if (idle > 20)
        {
          idle = 20;
        }

      usleep(idle * 1000);
    }

  return OK;
}
