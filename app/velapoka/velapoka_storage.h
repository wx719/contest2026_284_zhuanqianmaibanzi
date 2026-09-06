/****************************************************************************
 * apps/app/velapoka/velapoka_storage.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_VELAPOKA_VELAPOKA_STORAGE_H
#define __APPS_EXAMPLES_VELAPOKA_VELAPOKA_STORAGE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "velapoka_model.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VELAPOKA_HISTORY_COUNT 3

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct velapoka_storage_s;

struct velapoka_history_s
{
  uint32_t id;
  uint16_t score_tenths;
  bool pass;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int velapoka_storage_start(FAR struct velapoka_storage_s **storage);
int velapoka_storage_stop(FAR struct velapoka_storage_s *storage);
bool velapoka_storage_online(FAR struct velapoka_storage_s *storage);
int velapoka_storage_last_error(FAR struct velapoka_storage_s *storage);
int velapoka_storage_load_reference(
  FAR struct velapoka_storage_s *storage, FAR uint8_t *reference,
  size_t size, FAR unsigned int *threshold_percent);
int velapoka_storage_save_reference(
  FAR struct velapoka_storage_s *storage, FAR const uint8_t *reference,
  size_t size, unsigned int threshold_percent);
int velapoka_storage_save_result(
  FAR struct velapoka_storage_s *storage,
  FAR const struct velapoka_result_s *result,
  FAR const uint8_t *gray, size_t size, unsigned int threshold_percent,
  FAR uint32_t *record_id);
int velapoka_storage_read_export(
  FAR struct velapoka_storage_s *storage, FAR const char *relative_path,
  FAR uint8_t **data, FAR size_t *size);
unsigned int velapoka_storage_get_history(
  FAR struct velapoka_storage_s *storage,
  FAR struct velapoka_history_s *history, unsigned int capacity);

#endif /* __APPS_EXAMPLES_VELAPOKA_VELAPOKA_STORAGE_H */
