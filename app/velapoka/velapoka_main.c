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
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/video/fb.h>

#include <lvgl/lvgl.h>

#include "velapoka_camera.h"
#include "velapoka_inspect.h"
#include "velapoka_network.h"
#include "velapoka_state.h"
#include "velapoka_storage.h"
#include "velapoka_ui.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static volatile sig_atomic_t g_velapoka_exit_requested;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void velapoka_signal_handler(int signo)
{
  (void)signo;
  g_velapoka_exit_requested = 1;
}

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
  FAR struct velapoka_network_s *network = NULL;
  FAR struct velapoka_storage_s *storage = NULL;
  FAR uint8_t *storage_image = NULL;
  struct velapoka_history_s history[VELAPOKA_HISTORY_COUNT];
  struct velapoka_state_s state;
  unsigned int restored_threshold = 18;
  unsigned int history_count;
  bool touch_online;
  bool camera_online;
  bool storage_online;
  bool previous_storage_online;
  bool reference_restored = false;
  int sync_fd = -1;
  int storage_ret = OK;
  int ret;

  g_velapoka_exit_requested = 0;
  signal(SIGINT, velapoka_signal_handler);
  signal(SIGTERM, velapoka_signal_handler);
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

  ret = velapoka_inspect_start(&inspector);
  if (ret < 0)
    {
      fprintf(stderr, "velapoka: warning: inspector unavailable: %d\n",
              ret);
    }

  ret = velapoka_storage_start(&storage);
  if (ret < 0)
    {
      fprintf(stderr, "velapoka: warning: storage unavailable: %d\n",
              ret);
    }
  else
    {
      storage_image = malloc(VELAPOKA_INSPECT_SIZE);
      if (storage_image == NULL)
        {
          fprintf(stderr,
                  "velapoka: warning: storage image buffer unavailable\n");
          velapoka_storage_stop(storage);
          storage = NULL;
        }
      else if (inspector != NULL)
        {
          ret = velapoka_storage_load_reference(
            storage, storage_image, VELAPOKA_INSPECT_SIZE,
            &restored_threshold);
          if (ret == OK)
            {
              ret = velapoka_inspect_reference_import(
                inspector, storage_image, VELAPOKA_INSPECT_SIZE);
              reference_restored = ret == OK;
              if (ret < 0)
                {
                  fprintf(stderr,
                          "velapoka: warning: template restore failed: %d\n",
                          ret);
                }
            }
          else if (ret != -ENOENT)
            {
              fprintf(stderr,
                      "velapoka: warning: stored template invalid: %d\n",
                      ret);
            }
        }
    }

  ret = velapoka_camera_start(&camera);
  camera_online = ret == OK;
  if (!camera_online)
    {
      fprintf(stderr, "velapoka: warning: cannot start %s: %d\n",
              CONFIG_EXAMPLES_VELAPOKA_CAMERA_DEVPATH, ret);
    }

  storage_online = velapoka_storage_online(storage);
  previous_storage_online = storage_online;

  lv_display_set_default(result.disp);
  sync_fd = open(info.fb_path, O_RDWR | O_CLOEXEC);
  if (sync_fd < 0)
    {
      fprintf(stderr, "velapoka: warning: cannot sync to %s: %d\n",
              info.fb_path, errno);
    }

  ret = velapoka_ui_create(touch_online, camera_online, storage_online);
  if (ret < 0)
    {
      fprintf(stderr, "velapoka: UI creation failed: %d\n", ret);
      if (sync_fd >= 0)
        {
          close(sync_fd);
        }

      free(storage_image);
      velapoka_storage_stop(storage);
      velapoka_inspect_stop(inspector);
      velapoka_camera_stop(camera);
      lv_nuttx_deinit(&result);
      lv_deinit();
      return ret;
    }

  if (reference_restored)
    {
      velapoka_ui_set_threshold(restored_threshold);
    }

  ret = velapoka_network_start(&network, storage);
  if (ret < 0)
    {
      fprintf(stderr, "velapoka: warning: network unavailable: %d\n",
              ret);
    }

  history_count = velapoka_storage_get_history(
    storage, history, VELAPOKA_HISTORY_COUNT);
  velapoka_ui_set_history(history, history_count);

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
  else if (reference_restored)
    {
      velapoka_set_state(&state, VELAPOKA_STATE_READY);
      velapoka_ui_set_message("TEMPLATE RESTORED",
                              "Reference and threshold loaded from SD",
                              false);
    }

  while (!g_velapoka_exit_requested)
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
      else if (action == VELAPOKA_UI_ACTION_EXIT)
        {
          velapoka_ui_set_message("SHUTTING DOWN",
                                  "Flushing storage and unmounting SD",
                                  false);
          lv_timer_handler();
          break;
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
                          if (storage != NULL && storage_image != NULL)
                            {
                              ret = velapoka_inspect_reference_export(
                                inspector, storage_image,
                                VELAPOKA_INSPECT_SIZE);
                              if (ret == OK)
                                {
                                  ret = velapoka_storage_save_reference(
                                    storage, storage_image,
                                    VELAPOKA_INSPECT_SIZE,
                                    velapoka_ui_get_threshold());
                                }
                            }
                          else
                            {
                              ret = -ENODEV;
                            }

                          if (ret == OK)
                            {
                              velapoka_ui_set_message(
                                "SAMPLE READY",
                                "Reference queued for SD storage", false);
                            }
                          else
                            {
                              fprintf(stderr,
                                      "velapoka: template save failed: %d\n",
                                      ret);
                              velapoka_ui_set_message(
                                "SAMPLE READY",
                                "Reference active; SD save failed", true);
                            }

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
              uint32_t record_id;

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

              if (storage != NULL)
                {
                  if (!inspection.pass)
                    {
                      ret = velapoka_inspect_snapshot(
                        inspector, storage_image, VELAPOKA_INSPECT_SIZE);
                    }
                  else
                    {
                      ret = OK;
                    }

                  if (ret == OK)
                    {
                      ret = velapoka_storage_save_result(
                        storage, &inspection,
                        inspection.pass ? NULL : storage_image,
                        inspection.pass ? 0 : VELAPOKA_INSPECT_SIZE,
                        velapoka_ui_get_threshold(), &record_id);
                    }

                  if (ret == OK)
                    {
                      history_count = velapoka_storage_get_history(
                        storage, history, VELAPOKA_HISTORY_COUNT);
                      velapoka_ui_set_history(history, history_count);
                      printf("velapoka: result queued as record #%" PRIu32
                             "\n", record_id);
                    }
                  else
                    {
                      fprintf(stderr,
                              "velapoka: result storage failed: %d\n", ret);
                    }
                }

              velapoka_set_state(&state, VELAPOKA_STATE_SAVE_RESULT);
              velapoka_set_state(&state, VELAPOKA_STATE_READY);
            }
        }

      storage_online = velapoka_storage_online(storage);
      if (storage_online != previous_storage_online)
        {
          velapoka_ui_set_storage_online(storage_online);
          previous_storage_online = storage_online;
          if (!storage_online)
            {
              fprintf(stderr, "velapoka: storage went offline: %d\n",
                      velapoka_storage_last_error(storage));
            }
        }

      velapoka_network_update(
        network, state.current, camera != NULL, storage_online,
        inspector != NULL && velapoka_inspect_reference_ready(inspector),
        velapoka_ui_get_threshold());

      if (sync_fd >= 0)
        {
          do
            {
              ret = ioctl(sync_fd, FBIO_WAITFORVSYNC, 0);
            }
          while (ret < 0 && errno == EINTR &&
                 !g_velapoka_exit_requested);

          if (ret < 0 &&
              !(errno == EINTR && g_velapoka_exit_requested))
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

  printf("velapoka: shutdown requested\n");
  ret = velapoka_network_stop(network);
  if (ret < 0)
    {
      fprintf(stderr, "velapoka: network shutdown error: %d\n", ret);
    }

  velapoka_camera_stop(camera);
  velapoka_inspect_stop(inspector);
  storage_ret = velapoka_storage_stop(storage);

  if (storage_ret < 0)
    {
      velapoka_ui_set_message("SHUTDOWN ERROR",
                              "Storage flush failed; keep SD inserted",
                              true);
    }
  else
    {
      velapoka_ui_set_message("SAFE EXIT COMPLETE",
                              "Storage saved; safe to power off",
                              false);
    }

  lv_timer_handler();
  usleep(200 * 1000);

  if (sync_fd >= 0)
    {
      close(sync_fd);
    }

  free(storage_image);
  lv_nuttx_deinit(&result);
  lv_deinit();

  if (storage_ret < 0)
    {
      fprintf(stderr,
              "velapoka: shutdown completed with storage error: %d\n",
              storage_ret);
      return storage_ret;
    }

  printf("velapoka: safe shutdown complete\n");
  return OK;
}
