/* SPDX-License-Identifier: Apache-2.0 */

#ifndef __BOARDS_RISCV_ESP32P4_CONTEST_BOARD_SRC_SD_PROTOCOL_DEFS_H
#define __BOARDS_RISCV_ESP32P4_CONTEST_BOARD_SRC_SD_PROTOCOL_DEFS_H

#include <stddef.h>

#include <sys/lock.h>

#include "sd_protocol_types.h"

#ifndef __containerof
#  define __containerof(ptr, type, member) \
    ((FAR type *)((FAR uint8_t *)(ptr) - offsetof(type, member)))
#endif

/* Command indices consumed by the generic SDMMC transaction engine. */

#define MMC_GO_IDLE_STATE           0
#define MMC_READ_DAT_UNTIL_STOP    11
#define MMC_STOP_TRANSMISSION      12
#define MMC_READ_BLOCK_MULTIPLE    18
#define MMC_WRITE_DAT_UNTIL_STOP   20
#define MMC_WRITE_BLOCK_MULTIPLE   25
#define MMC_APP_CMD                55
#define SD_SWITCH_VOLTAGE          11

#endif
