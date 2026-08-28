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

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int velapoka_ui_create(bool touch_online, bool camera_online);
int velapoka_ui_update_preview(FAR const uint8_t *gray, size_t size,
                               uint32_t sequence);
void velapoka_ui_set_camera_online(bool online);

#endif /* __APPS_EXAMPLES_VELAPOKA_VELAPOKA_UI_H */
