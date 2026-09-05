/*
 * SPDX-FileCopyrightText: 2006 Uwe Stuehler <uwe@openbsd.org>
 * SPDX-FileContributor: 2016-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ISC
 */

#ifndef __BOARDS_RISCV_ESP32P4_CONTEST_BOARD_SRC_SD_PROTOCOL_TYPES_H
#define __BOARDS_RISCV_ESP32P4_CONTEST_BOARD_SRC_SD_PROTOCOL_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Minimal SD protocol definitions required by esp-hal-3rdparty's SD host.
 * The full ESP-IDF card stack is deliberately not imported: ESP-Hosted only
 * needs synchronous SDIO commands and performs no block-card operations.
 */

typedef uint32_t sdmmc_response_t[4];

typedef struct
{
  uint32_t opcode;
  uint32_t arg;
  sdmmc_response_t response;
  void *data;
  size_t datalen;
  size_t buflen;
  size_t blklen;
  int flags;
#define SCF_ITSDONE      0x0001
#define SCF_CMD(flags)   ((flags) & 0x00f0)
#define SCF_CMD_AC       0x0000
#define SCF_CMD_ADTC     0x0010
#define SCF_CMD_BC       0x0020
#define SCF_CMD_BCR      0x0030
#define SCF_CMD_READ     0x0040
#define SCF_RSP_BSY      0x0100
#define SCF_RSP_136      0x0200
#define SCF_RSP_CRC      0x0400
#define SCF_RSP_IDX      0x0800
#define SCF_RSP_PRESENT  0x1000
#define SCF_RSP_R0       0
#define SCF_RSP_R1       (SCF_RSP_PRESENT | SCF_RSP_CRC | SCF_RSP_IDX)
#define SCF_RSP_R1B      (SCF_RSP_R1 | SCF_RSP_BSY)
#define SCF_RSP_R2       (SCF_RSP_PRESENT | SCF_RSP_CRC | SCF_RSP_136)
#define SCF_RSP_R3       SCF_RSP_PRESENT
#define SCF_RSP_R4       SCF_RSP_PRESENT
#define SCF_RSP_R5       SCF_RSP_R1
#define SCF_RSP_R5B      (SCF_RSP_R5 | SCF_RSP_BSY)
#define SCF_RSP_R6       SCF_RSP_R1
#define SCF_RSP_R7       SCF_RSP_R1
#define SCF_WAIT_BUSY    0x2000
  esp_err_t error;
  uint32_t timeout_ms;
  esp_err_t (*volt_switch_cb)(void *arg, int stage);
  void *volt_switch_cb_arg;
} sdmmc_command_t;

#define SDMMC_FREQ_DEFAULT    20000
#define SDMMC_FREQ_HIGHSPEED  40000
#define SDMMC_FREQ_PROBING      400
#define SDMMC_FREQ_SDR50     100000
#define SDMMC_FREQ_SDR104    200000

#endif
