/****************************************************************************
 * apps/examples/velapoka/velapoka_inspect.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_VELAPOKA_VELAPOKA_INSPECT_H
#define __APPS_EXAMPLES_VELAPOKA_VELAPOKA_INSPECT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "velapoka_model.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct velapoka_inspector_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int velapoka_inspect_start(FAR struct velapoka_inspector_s **inspector);
void velapoka_inspect_stop(FAR struct velapoka_inspector_s *inspector);
int velapoka_inspect_enroll_begin(
  FAR struct velapoka_inspector_s *inspector);
int velapoka_inspect_enroll_frame(
  FAR struct velapoka_inspector_s *inspector,
  FAR const uint8_t *preview, size_t size,
  FAR unsigned int *captured, FAR bool *ready);
bool velapoka_inspect_reference_ready(
  FAR struct velapoka_inspector_s *inspector);
int velapoka_inspect_reference_export(
  FAR struct velapoka_inspector_s *inspector,
  FAR uint8_t *reference, size_t size);
int velapoka_inspect_reference_import(
  FAR struct velapoka_inspector_s *inspector,
  FAR const uint8_t *reference, size_t size);
int velapoka_inspect_submit(FAR struct velapoka_inspector_s *inspector,
                            FAR const uint8_t *preview, size_t size,
                            uint32_t sequence,
                            unsigned int threshold_percent);
int velapoka_inspect_poll(FAR struct velapoka_inspector_s *inspector,
                          FAR struct velapoka_result_s *result);
int velapoka_inspect_snapshot(FAR struct velapoka_inspector_s *inspector,
                              FAR uint8_t *normalized, size_t size);
void velapoka_inspect_discard_result(
  FAR struct velapoka_inspector_s *inspector);

#endif /* __APPS_EXAMPLES_VELAPOKA_VELAPOKA_INSPECT_H */
