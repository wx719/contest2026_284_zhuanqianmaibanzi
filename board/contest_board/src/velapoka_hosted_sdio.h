/* SPDX-License-Identifier: Apache-2.0 */

#ifndef __BOARDS_RISCV_ESP32P4_CONTEST_BOARD_SRC_VELAPOKA_HOSTED_SDIO_H
#define __BOARDS_RISCV_ESP32P4_CONTEST_BOARD_SRC_VELAPOKA_HOSTED_SDIO_H

#include <stdbool.h>
#include <stdint.h>

void *velapoka_hosted_sdio_initialize(void);
int velapoka_hosted_sdio_card_initialize(void *context);
int velapoka_hosted_sdio_read(uint32_t address, uint8_t *buffer,
                              uint16_t size, bool increment);
int velapoka_hosted_sdio_write(uint32_t address, const uint8_t *buffer,
                               uint16_t size, bool increment);
int velapoka_hosted_sdio_wait_interrupt(uint32_t timeout_ticks);

#endif
