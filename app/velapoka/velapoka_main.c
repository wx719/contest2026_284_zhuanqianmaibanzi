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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <lvgl/lvgl.h>

#include "velapoka_ui.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
  bool touch_online;
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

  lv_display_set_default(result.disp);
  ret = velapoka_ui_create(touch_online);
  if (ret < 0)
    {
      fprintf(stderr, "velapoka: UI creation failed: %d\n", ret);
      lv_nuttx_deinit(&result);
      lv_deinit();
      return ret;
    }

  printf("velapoka: LVGL first-light ready, fb=%s touch=%s\n",
         info.fb_path, touch_online ? info.input_path : "offline");

  for (; ; )
    {
      uint32_t idle = lv_timer_handler();

      if (idle < 1)
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
