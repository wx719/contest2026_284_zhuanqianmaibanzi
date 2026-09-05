/****************************************************************************
 * vendor/openvela/boards/contest2026_284_board/src/velapoka_hosted_sdio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/clock.h>
#include <nuttx/compiler.h>
#include <nuttx/mutex.h>
#include <nuttx/signal.h>

#include <arch/board/board.h>

#include "driver/sd_host.h"
#include "driver/sd_host_sdmmc.h"
#include "sd_protocol_types.h"
#include "velapoka_hosted_sdio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SDIO_CMD_GO_IDLE             0
#define SDIO_CMD_IO_SEND_OP_COND     5
#define SDIO_CMD_SEND_RELATIVE_ADDR  3
#define SDIO_CMD_SELECT_CARD         7
#define SDIO_CMD_IO_RW_DIRECT       52
#define SDIO_CMD_IO_RW_EXTENDED     53

#define SDIO_OCR_READY               (1u << 31)
#define SDIO_OCR_VOLTAGE_MASK        0x00ff8000u
#define SDIO_R5_ERROR_MASK           0x0000cb00u

#define SDIO_CCCR_IO_ENABLE          0x02
#define SDIO_CCCR_IO_READY           0x03
#define SDIO_CCCR_INT_ENABLE         0x04
#define SDIO_CCCR_IO_ABORT           0x06
#define SDIO_CCCR_BUS_INTERFACE      0x07
#define SDIO_CCCR_FN0_BLOCK_LOW      0x10
#define SDIO_CCCR_FN0_BLOCK_HIGH     0x11
#define SDIO_FBR_FN1                 0x100
#define SDIO_FBR_BLOCK_LOW           0x10
#define SDIO_FBR_BLOCK_HIGH          0x11

#define SDIO_FN1                     1
#define SDIO_FN1_ENABLE              (1u << SDIO_FN1)
#define SDIO_INT_MASTER_ENABLE       (1u << 0)
#define SDIO_BUS_WIDTH_4             (1u << 1)
#define SDIO_IO_RESET                (1u << 3)
#define SDIO_BLOCK_SIZE              512
#define SDIO_DMA_ALIGNMENT           CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define SDIO_DMA_BUFFER_SIZE         1536
#define SDIO_INIT_RETRIES            100

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct velapoka_sdio_s
{
  sd_host_ctlr_handle_t controller;
  sd_host_slot_handle_t slot;
  mutex_t lock;
  uint16_t rca;
  bool card_ready;
  uint8_t dma_buffer[SDIO_DMA_BUFFER_SIZE]
    aligned_data(SDIO_DMA_ALIGNMENT);
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct velapoka_sdio_s g_sdio =
{
  .lock = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int velapoka_sdio_command(uint32_t opcode, uint32_t argument,
                                 int flags, void *data, size_t length,
                                 size_t block_length, uint32_t *response)
{
  sdmmc_command_t command;
  esp_err_t err;

  memset(&command, 0, sizeof(command));
  command.opcode = opcode;
  command.arg = argument;
  command.flags = flags;
  command.data = data;
  command.datalen = length;
  command.buflen = data == NULL ? length :
                   (length + SDIO_DMA_ALIGNMENT - 1) &
                   ~(SDIO_DMA_ALIGNMENT - 1);
  command.blklen = block_length;
  command.timeout_ms = 1000;

  err = sd_host_slot_do_transaction(g_sdio.slot, &command);
  if (err != ESP_OK)
    {
      return err == ESP_ERR_TIMEOUT ? -ETIMEDOUT : -EIO;
    }

  if (response != NULL)
    {
      *response = command.response[0];
    }

  return OK;
}

static int velapoka_sdio_cmd52(bool write, uint8_t function,
                               uint32_t address, uint8_t input,
                               uint8_t *output)
{
  uint32_t argument;
  uint32_t response;
  int ret;

  argument = (write ? 1u << 31 : 0) |
             ((uint32_t)(function & 7) << 28) |
             ((uint32_t)(address & 0x1ffff) << 9) | input;
  if (write && output != NULL)
    {
      argument |= 1u << 27;
    }

  ret = velapoka_sdio_command(SDIO_CMD_IO_RW_DIRECT, argument,
                              SCF_CMD_AC | SCF_RSP_R5, NULL, 0, 0,
                              &response);
  if (ret < 0)
    {
      return ret;
    }

  if ((response & SDIO_R5_ERROR_MASK) != 0)
    {
      return -EIO;
    }

  if (output != NULL)
    {
      *output = response & 0xff;
    }

  return OK;
}

static int velapoka_sdio_set_block_size(uint8_t function, uint16_t size)
{
  uint32_t base = function == 0 ? 0 : SDIO_FBR_FN1;
  int ret;

  ret = velapoka_sdio_cmd52(true, 0, base + SDIO_FBR_BLOCK_LOW,
                            size & 0xff, NULL);
  if (ret == OK)
    {
      ret = velapoka_sdio_cmd52(true, 0, base + SDIO_FBR_BLOCK_HIGH,
                                size >> 8, NULL);
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void *velapoka_hosted_sdio_initialize(void)
{
  sd_host_sdmmc_cfg_t controller_config =
    {
      .event_queue_items = 8,
      .dma_desc_num = 4,
    };

  sd_host_slot_sdmmc_init_cfg_t slot_config =
    {
      .slot_id = 1,
      .sd_mode = SD_MODE_NORMAL,
      .io_config =
        {
          /* Describe all four routed data pins.  The SD host keeps the bus
           * in one-bit INIT state until card enumeration switches it to
           * four-bit.
           */

          .width = SD_BUS_WIDTH_4_BIT,
          .clk_io = BOARD_VELAPOKA_WIFI_SDIO_CLK,
          .cmd_io = BOARD_VELAPOKA_WIFI_SDIO_CMD,
          .cd_io = GPIO_NUM_NC,
          .wp_io = GPIO_NUM_NC,
          .d0_io = BOARD_VELAPOKA_WIFI_SDIO_D0,
          .d1_io = BOARD_VELAPOKA_WIFI_SDIO_D1,
          .d2_io = BOARD_VELAPOKA_WIFI_SDIO_D2,
          .d3_io = BOARD_VELAPOKA_WIFI_SDIO_D3,
          .d4_io = GPIO_NUM_NC,
          .d5_io = GPIO_NUM_NC,
          .d6_io = GPIO_NUM_NC,
          .d7_io = GPIO_NUM_NC,
        },

      .slot_flags.internal_pullup = true,
    };

  esp_err_t err;

  if (g_sdio.slot != NULL)
    {
      return &g_sdio;
    }

  err = sd_host_create_sdmmc_controller(&controller_config,
                                         &g_sdio.controller);
  if (err != ESP_OK)
    {
      syslog(LOG_ERR, "ERROR: ESP-Hosted SDMMC controller: %d\n", err);
      return NULL;
    }

  err = sd_host_sdmmc_controller_add_slot(g_sdio.controller, &slot_config,
                                           &g_sdio.slot);
  if (err != ESP_OK)
    {
      syslog(LOG_ERR, "ERROR: ESP-Hosted SDIO slot: %d\n", err);
      return NULL;
    }

  return &g_sdio;
}

int velapoka_hosted_sdio_card_initialize(void *context)
{
  sd_host_slot_cfg_t bus_config;
  uint32_t response;
  uint32_t ocr;
  uint8_t value;
  int retry;
  int ret;

  if (context != &g_sdio || g_sdio.slot == NULL)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_sdio.lock);
  if (g_sdio.card_ready)
    {
      nxmutex_unlock(&g_sdio.lock);
      return OK;
    }

  /* Follow ESP-IDF's sdmmc_card_init() ordering.  ESP-Hosted already reset
   * the C6 through GPIO54 before opening this transport, so do not reset it
   * a second time here.  Reset the SDIO function itself with CMD52 before
   * issuing CMD0/CMD5.  A timeout is permitted by the official stack because
   * the function can reset before returning its response; CMD5 below is the
   * authoritative presence/readiness check.
   */

  ret = velapoka_sdio_cmd52(true, 0, SDIO_CCCR_IO_ABORT,
                            SDIO_IO_RESET, NULL);
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "ESP-Hosted: CMD52 SDIO reset returned %d, continuing\n",
             ret);
    }

  ret = velapoka_sdio_command(SDIO_CMD_GO_IDLE, 0,
                              SCF_CMD_BC | SCF_RSP_R0, NULL, 0, 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: ESP-Hosted CMD0 failed: %d\n", ret);
      goto out;
    }

  ret = velapoka_sdio_command(SDIO_CMD_IO_SEND_OP_COND, 0,
                              SCF_CMD_BCR | SCF_RSP_R4, NULL, 0, 0,
                              &response);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: ESP-Hosted CMD5 probe failed: %d\n", ret);
      goto out;
    }

  syslog(LOG_INFO, "ESP-Hosted: CMD5 OCR=%08" PRIx32 " functions=%u\n",
         response, (unsigned int)((response >> 28) & 7));

  ocr = response & SDIO_OCR_VOLTAGE_MASK;
  for (retry = 0; retry < SDIO_INIT_RETRIES; retry++)
    {
      ret = velapoka_sdio_command(SDIO_CMD_IO_SEND_OP_COND, ocr,
                                  SCF_CMD_BCR | SCF_RSP_R4, NULL, 0, 0,
                                  &response);
      if (ret < 0 || (response & SDIO_OCR_READY) != 0)
        {
          break;
        }

      nxsig_usleep(10000);
    }

  if (ret < 0 || (response & SDIO_OCR_READY) == 0)
    {
      ret = ret < 0 ? ret : -ETIMEDOUT;
      syslog(LOG_ERR,
             "ERROR: ESP-Hosted CMD5 not ready: ret=%d OCR=%08" PRIx32
             " retries=%d\n",
             ret, response, retry);
      goto out;
    }

  ret = sd_host_slot_enable_io_int(g_sdio.slot);
  if (ret != ESP_OK)
    {
      syslog(LOG_ERR,
             "ERROR: ESP-Hosted SDIO interrupt enable failed: %d\n",
             ret);
      ret = -EIO;
      goto out;
    }

  ret = velapoka_sdio_command(SDIO_CMD_SEND_RELATIVE_ADDR, 0,
                              SCF_CMD_BCR | SCF_RSP_R6, NULL, 0, 0,
                              &response);
  if (ret < 0)
    {
      goto out;
    }

  g_sdio.rca = response >> 16;
  ret = velapoka_sdio_command(SDIO_CMD_SELECT_CARD,
                              (uint32_t)g_sdio.rca << 16,
                              SCF_CMD_AC | SCF_RSP_R1, NULL, 0, 0, NULL);
  if (ret < 0)
    {
      goto out;
    }

  ret = velapoka_sdio_cmd52(false, 0, SDIO_CCCR_IO_ENABLE, 0, &value);
  if (ret == OK)
    {
      ret = velapoka_sdio_cmd52(true, 0, SDIO_CCCR_IO_ENABLE,
                                value | SDIO_FN1_ENABLE, NULL);
    }

  for (retry = 0; ret == OK && retry < SDIO_INIT_RETRIES; retry++)
    {
      ret = velapoka_sdio_cmd52(false, 0, SDIO_CCCR_IO_READY, 0, &value);
      if (ret < 0 || (value & SDIO_FN1_ENABLE) != 0)
        {
          break;
        }

      nxsig_usleep(10000);
    }

  if (ret < 0 || (value & SDIO_FN1_ENABLE) == 0)
    {
      ret = ret < 0 ? ret : -ETIMEDOUT;
      goto out;
    }

  ret = velapoka_sdio_cmd52(false, 0, SDIO_CCCR_INT_ENABLE, 0, &value);
  if (ret == OK)
    {
      ret = velapoka_sdio_cmd52(true, 0, SDIO_CCCR_INT_ENABLE,
                                value | SDIO_INT_MASTER_ENABLE |
                                SDIO_FN1_ENABLE, NULL);
    }

  if (ret == OK)
    {
      ret = velapoka_sdio_set_block_size(0, SDIO_BLOCK_SIZE);
    }

  if (ret == OK)
    {
      ret = velapoka_sdio_set_block_size(1, SDIO_BLOCK_SIZE);
    }

  if (ret == OK)
    {
      ret = velapoka_sdio_cmd52(false, 0, SDIO_CCCR_BUS_INTERFACE, 0,
                                &value);
    }

  if (ret == OK)
    {
      value = (value & ~3u) | SDIO_BUS_WIDTH_4;
      ret = velapoka_sdio_cmd52(true, 0, SDIO_CCCR_BUS_INTERFACE, value,
                                NULL);
    }

  if (ret == OK)
    {
      memset(&bus_config, 0, sizeof(bus_config));
      bus_config.width = SD_BUS_WIDTH_4_BIT;
      bus_config.freq_hz = CONFIG_VELAPOKA_WIFI_SDIO_FREQUENCY;
      ret = sd_host_slot_configure(g_sdio.slot, &bus_config) == ESP_OK ?
            OK : -EIO;
    }

  if (ret == OK)
    {
      g_sdio.card_ready = true;
      syslog(LOG_INFO, "ESP-Hosted: SDIO function 1 ready, %u Hz, 4-bit\n",
             CONFIG_VELAPOKA_WIFI_SDIO_FREQUENCY);
    }

out:
  nxmutex_unlock(&g_sdio.lock);
  return ret;
}

static int velapoka_sdio_transfer_once(bool write, uint32_t address,
                                       uint8_t *buffer, uint16_t size,
                                       size_t block_length, bool block_mode,
                                       bool increment)
{
  uint8_t *dma_buffer = g_sdio.dma_buffer;
  uint32_t argument;
  uint32_t response;
  uint16_t count;
  int flags;
  int ret;

  if (size > sizeof(g_sdio.dma_buffer))
    {
      return -E2BIG;
    }

  if (write)
    {
      memcpy(dma_buffer, buffer, size);
    }

  count = block_mode ? size / SDIO_BLOCK_SIZE : size;
  argument = (write ? 1u << 31 : 0) | ((uint32_t)SDIO_FN1 << 28) |
             (block_mode ? 1u << 27 : 0) |
             (increment ? 1u << 26 : 0) |
             ((address & 0x1ffff) << 9) | (count & 0x1ff);
  flags = SCF_CMD_ADTC | SCF_RSP_R5;
  if (!write)
    {
      flags |= SCF_CMD_READ;
    }

  ret = velapoka_sdio_command(SDIO_CMD_IO_RW_EXTENDED, argument, flags,
                              dma_buffer, size, block_length, &response);
  if (ret == OK && (response & SDIO_R5_ERROR_MASK) != 0)
    {
      ret = -EIO;
    }

  if (ret == OK && !write)
    {
      memcpy(buffer, dma_buffer, size);
    }

  return ret;
}

static int velapoka_sdio_transfer(bool write, uint32_t address,
                                  uint8_t *buffer, uint16_t size,
                                  bool increment)
{
  uint16_t transfer;
  int ret = OK;

  if (!g_sdio.card_ready || buffer == NULL || size == 0)
    {
      return -EINVAL;
    }

  /* CMD53 block mode can only describe complete 512-byte blocks.  Match
   * ESP-Hosted's reference wrapper by transferring all complete blocks
   * first and the tail with byte mode.
   */

  while (size >= SDIO_BLOCK_SIZE)
    {
      transfer = (size / SDIO_BLOCK_SIZE) * SDIO_BLOCK_SIZE;
      if (transfer > sizeof(g_sdio.dma_buffer))
        {
          transfer = sizeof(g_sdio.dma_buffer);
        }

      ret = velapoka_sdio_transfer_once(write, address, buffer, transfer,
                                        SDIO_BLOCK_SIZE, true, increment);
      if (ret < 0)
        {
          return ret;
        }

      size -= transfer;
      buffer += transfer;
      if (increment)
        {
          address += transfer;
        }
    }

  if (size > 0)
    {
      ret = velapoka_sdio_transfer_once(write, address, buffer, size, size,
                                        false, increment);
    }

  return ret;
}

int velapoka_hosted_sdio_read(uint32_t address, uint8_t *buffer,
                              uint16_t size, bool increment)
{
  int ret;

  nxmutex_lock(&g_sdio.lock);
  ret = size == 1 ? velapoka_sdio_cmd52(false, SDIO_FN1, address, 0,
                                        buffer) :
                    velapoka_sdio_transfer(false, address, buffer, size,
                                           increment);
  nxmutex_unlock(&g_sdio.lock);
  return ret;
}

int velapoka_hosted_sdio_write(uint32_t address, const uint8_t *buffer,
                               uint16_t size, bool increment)
{
  int ret;

  nxmutex_lock(&g_sdio.lock);
  ret = size == 1 ? velapoka_sdio_cmd52(true, SDIO_FN1, address, buffer[0],
                                        NULL) :
                    velapoka_sdio_transfer(true, address,
                                           (uint8_t *)buffer, size,
                                           increment);
  nxmutex_unlock(&g_sdio.lock);
  return ret;
}

int velapoka_hosted_sdio_wait_interrupt(uint32_t timeout_ticks)
{
  esp_err_t err;

  err = sd_host_slot_wait_io_int(g_sdio.slot, timeout_ticks);
  if (err == ESP_OK)
    {
      return OK;
    }

  return err == ESP_ERR_TIMEOUT ? -ETIMEDOUT : -EIO;
}
