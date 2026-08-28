/****************************************************************************
 * apps/app/velapoka/velapoka_camera.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_VELAPOKA_VELAPOKA_CAMERA_H
#define __APPS_EXAMPLES_VELAPOKA_VELAPOKA_CAMERA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VELAPOKA_CAMERA_PREVIEW_WIDTH  512
#define VELAPOKA_CAMERA_PREVIEW_HEIGHT 288
#define VELAPOKA_CAMERA_PREVIEW_SIZE \
  (VELAPOKA_CAMERA_PREVIEW_WIDTH * VELAPOKA_CAMERA_PREVIEW_HEIGHT)

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct velapoka_camera_s;

struct velapoka_camera_frame_s
{
  FAR const uint8_t *data;
  size_t size;
  uint32_t sequence;
  uint8_t index;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int velapoka_camera_start(FAR struct velapoka_camera_s **camera);
int velapoka_camera_acquire(FAR struct velapoka_camera_s *camera,
                            FAR struct velapoka_camera_frame_s *frame);
void velapoka_camera_release(
  FAR struct velapoka_camera_s *camera,
  FAR const struct velapoka_camera_frame_s *frame);
void velapoka_camera_stop(FAR struct velapoka_camera_s *camera);

#endif /* __APPS_EXAMPLES_VELAPOKA_VELAPOKA_CAMERA_H */
