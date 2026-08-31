/****************************************************************************
 * apps/examples/velapoka/velapoka_model.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_VELAPOKA_VELAPOKA_MODEL_H
#define __APPS_EXAMPLES_VELAPOKA_VELAPOKA_MODEL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VELAPOKA_INSPECT_WIDTH       320
#define VELAPOKA_INSPECT_HEIGHT      180
#define VELAPOKA_INSPECT_SIZE \
  (VELAPOKA_INSPECT_WIDTH * VELAPOKA_INSPECT_HEIGHT)

#define VELAPOKA_ROI_X               48
#define VELAPOKA_ROI_Y               32
#define VELAPOKA_ROI_WIDTH           224
#define VELAPOKA_ROI_HEIGHT          112
#define VELAPOKA_INSPECT_BLOCK_SIZE  16
#define VELAPOKA_INSPECT_MAX_BOXES   8

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct velapoka_box_s
{
  uint16_t x;
  uint16_t y;
  uint16_t width;
  uint16_t height;
};

struct velapoka_result_s
{
  struct velapoka_box_s boxes[VELAPOKA_INSPECT_MAX_BOXES];
  uint32_t sequence;
  uint32_t elapsed_ms;
  uint16_t score_tenths;
  uint16_t difference_tenths;
  uint8_t box_count;
  bool pass;
};

#endif /* __APPS_EXAMPLES_VELAPOKA_VELAPOKA_MODEL_H */
