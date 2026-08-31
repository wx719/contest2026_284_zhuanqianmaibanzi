/****************************************************************************
 * apps/app/velapoka/velapoka_ui.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_VELAPOKA_VELAPOKA_UI_H
#define __APPS_EXAMPLES_VELAPOKA_VELAPOKA_UI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "velapoka_model.h"
#include "velapoka_state.h"
#include "velapoka_storage.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum velapoka_ui_action_e
{
  VELAPOKA_UI_ACTION_NONE = 0,
  VELAPOKA_UI_ACTION_ENROLL,
  VELAPOKA_UI_ACTION_INSPECT,
  VELAPOKA_UI_ACTION_STOP,
  VELAPOKA_UI_ACTION_EXIT
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int velapoka_ui_create(bool touch_online, bool camera_online,
                       bool storage_online);
int velapoka_ui_update_preview(FAR const uint8_t *gray, size_t size,
                               uint32_t sequence);
void velapoka_ui_set_camera_online(bool online);
void velapoka_ui_set_storage_online(bool online);
enum velapoka_ui_action_e velapoka_ui_take_action(void);
unsigned int velapoka_ui_get_threshold(void);
void velapoka_ui_set_threshold(unsigned int threshold_percent);
void velapoka_ui_set_state(enum velapoka_state_e state);
void velapoka_ui_set_enroll_progress(unsigned int captured,
                                     unsigned int total);
void velapoka_ui_set_message(FAR const char *title,
                             FAR const char *detail, bool error);
void velapoka_ui_set_result(FAR const struct velapoka_result_s *result);
void velapoka_ui_set_history(FAR const struct velapoka_history_s *history,
                             unsigned int count);
void velapoka_ui_clear_boxes(void);

#endif /* __APPS_EXAMPLES_VELAPOKA_VELAPOKA_UI_H */
