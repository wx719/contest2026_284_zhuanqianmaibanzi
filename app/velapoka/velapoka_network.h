/****************************************************************************
 * apps/app/velapoka/velapoka_network.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_VELAPOKA_VELAPOKA_NETWORK_H
#define __APPS_EXAMPLES_VELAPOKA_VELAPOKA_NETWORK_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>

#include "velapoka_state.h"
#include "velapoka_storage.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct velapoka_network_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int velapoka_network_start(
  FAR struct velapoka_network_s **network,
  FAR struct velapoka_storage_s *storage);
int velapoka_network_stop(FAR struct velapoka_network_s *network);
void velapoka_network_update(
  FAR struct velapoka_network_s *network, enum velapoka_state_e state,
  bool camera_online, bool storage_online, bool reference_ready,
  unsigned int threshold_percent);
bool velapoka_network_online(FAR struct velapoka_network_s *network);
int velapoka_network_last_error(FAR struct velapoka_network_s *network);

#endif /* __APPS_EXAMPLES_VELAPOKA_VELAPOKA_NETWORK_H */
