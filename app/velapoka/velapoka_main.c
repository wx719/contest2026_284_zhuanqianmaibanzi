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
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/video/fb.h>

#include <lvgl/lvgl.h>

#include "velapoka_camera.h"
#include "velapoka_inspect.h"
#include "velapoka_state.h"
#include "velapoka_ui.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int velapoka_set_state(FAR struct velapoka_state_s *state,
                              enum velapoka_state_e next)
{
  int ret = velapoka_state_transition(state, next);

  if (ret == OK)
    {
      velapoka_ui_set_state(next);
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
  FAR struct velapoka_camera_s *camera = NULL;
  FAR struct velapoka_inspector_s *inspector = NULL;
  struct velapoka_state_s state;
  bool touch_online;
  bool camera_online;
  int sync_fd = -1;
  int ret;

  velapoka_state_init(&state);

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

  ret = velapoka_inspect_start(&inspector);
  if (ret < 0)
    {
      fprintf(stderr, "velapoka: warning: inspector unavailable: %d\n",
              ret);
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
      velapoka_inspect_stop(inspector);
      velapoka_camera_stop(camera);
      lv_nuttx_deinit(&result);
      lv_deinit();
      return ret;
    }

  printf("velapoka: live preview ready, fb=%s touch=%s camera=%s\n",
         info.fb_path, touch_online ? info.input_path : "offline",
         camera_online ? CONFIG_EXAMPLES_VELAPOKA_CAMERA_DEVPATH :
                         "offline");

  velapoka_set_state(&state, VELAPOKA_STATE_SELF_TEST);
  velapoka_set_state(&state, VELAPOKA_STATE_IDLE);
  if (inspector == NULL)
    {
      velapoka_set_state(&state, VELAPOKA_STATE_ERROR);
      velapoka_ui_set_message("INSPECTOR ERROR",
                              "Preview only; restart the application",
                              true);
    }

  for (; ; )
    {
      enum velapoka_ui_action_e action;
      uint32_t idle;

      action = velapoka_ui_take_action();
      if (action == VELAPOKA_UI_ACTION_ENROLL)
        {
          if (camera == NULL || inspector == NULL)
            {
              velapoka_set_state(&state, VELAPOKA_STATE_ERROR);
              velapoka_ui_set_message("ENROLL ERROR",
                                      "Camera or inspector is offline",
                                      true);
            }
          else
            {
              ret = velapoka_inspect_enroll_begin(inspector);
              if (ret == OK)
                {
                  velapoka_set_state(&state,
                                     VELAPOKA_STATE_SAMPLE_CAPTURE);
                  velapoka_ui_clear_boxes();
                  velapoka_ui_set_enroll_progress(0, 3);
                }
              else
                {
                  velapoka_ui_set_message("ENROLL BUSY",
                                          "Wait for inspection to finish",
                                          true);
                }
            }
        }
      else if (action == VELAPOKA_UI_ACTION_INSPECT)
        {
          if (camera == NULL || inspector == NULL ||
              !velapoka_inspect_reference_ready(inspector))
            {
              velapoka_set_state(&state, VELAPOKA_STATE_ERROR);
              velapoka_ui_set_message("NO REFERENCE",
                                      "Enroll a standard sample first",
                                      true);
            }
          else if (state.current == VELAPOKA_STATE_INSPECT ||
                   state.current == VELAPOKA_STATE_PREPROCESS ||
                   state.current == VELAPOKA_STATE_CAPTURE)
            {
              velapoka_ui_set_message("INSPECT BUSY",
                                      "Current inspection is running",
                                      false);
            }
          else
            {
              if (state.current == VELAPOKA_STATE_IDLE ||
                  state.current == VELAPOKA_STATE_ERROR)
                {
                  velapoka_set_state(&state, VELAPOKA_STATE_READY);
                }

              velapoka_set_state(&state, VELAPOKA_STATE_CAPTURE);
              velapoka_ui_clear_boxes();
              velapoka_ui_set_message("CAPTURE",
                                      "Waiting for the next camera frame",
                                      false);
            }
        }
      else if (action == VELAPOKA_UI_ACTION_STOP)
        {
          velapoka_inspect_discard_result(inspector);
          velapoka_set_state(&state, VELAPOKA_STATE_IDLE);
          velapoka_ui_clear_boxes();
          velapoka_ui_set_message("STOPPED",
                                  "Enroll or inspect when ready", false);
        }

      if (camera != NULL)
        {
          struct velapoka_camera_frame_s frame;

          ret = velapoka_camera_acquire(camera, &frame);
          if (ret > 0)
            {
              ret = velapoka_ui_update_preview(frame.data, frame.size,
                                               frame.sequence);
              if (ret == OK && state.current ==
                               VELAPOKA_STATE_SAMPLE_CAPTURE)
                {
                  unsigned int captured;
                  bool ready;

                  ret = velapoka_inspect_enroll_frame(inspector,
                                                       frame.data,
                                                       frame.size,
                                                       &captured, &ready);
                  if (ret == OK)
                    {
                      velapoka_ui_set_enroll_progress(captured, 3);
                      if (ready)
                        {
                          velapoka_set_state(&state,
                                             VELAPOKA_STATE_SAVE_SAMPLE);
                          velapoka_ui_set_message("SAMPLE READY",
                                                  "3-frame reference active",
                                                  false);
                          velapoka_set_state(&state,
                                             VELAPOKA_STATE_READY);
                        }
                    }
                  else
                    {
                      velapoka_set_state(&state, VELAPOKA_STATE_ERROR);
                      velapoka_ui_set_message("ENROLL ERROR",
                                              "Reference capture failed",
                                              true);
                    }
                }
              else if (ret == OK &&
                       state.current == VELAPOKA_STATE_CAPTURE)
                {
                  velapoka_set_state(&state,
                                     VELAPOKA_STATE_PREPROCESS);
                  ret = velapoka_inspect_submit(
                    inspector, frame.data, frame.size, frame.sequence,
                    velapoka_ui_get_threshold());
                  if (ret == OK)
                    {
                      velapoka_set_state(&state,
                                         VELAPOKA_STATE_INSPECT);
                      velapoka_ui_set_message("INSPECTING",
                                              "P0 comparison is running",
                                              false);
                    }
                  else
                    {
                      velapoka_set_state(&state, VELAPOKA_STATE_ERROR);
                      velapoka_ui_set_message("INSPECT ERROR",
                                              "Could not queue the frame",
                                              true);
                    }
                }

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

      if (inspector != NULL)
        {
          struct velapoka_result_s inspection;

          ret = velapoka_inspect_poll(inspector, &inspection);
          if (ret > 0 && state.current == VELAPOKA_STATE_INSPECT)
            {
              velapoka_set_state(&state,
                                 inspection.pass ? VELAPOKA_STATE_PASS :
                                                   VELAPOKA_STATE_FAIL);
              velapoka_ui_set_result(&inspection);
              printf("velapoka: result=%s score=%u.%u diff=%u.%u%% "
                     "boxes=%u time=%" PRIu32 " ms sequence=%" PRIu32
                     "\n",
                     inspection.pass ? "PASS" : "FAIL",
                     inspection.score_tenths / 10,
                     inspection.score_tenths % 10,
                     inspection.difference_tenths / 10,
                     inspection.difference_tenths % 10,
                     inspection.box_count, inspection.elapsed_ms,
                     inspection.sequence);
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
