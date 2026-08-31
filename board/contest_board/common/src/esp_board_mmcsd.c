/****************************************************************************
 * vendor/openvela/boards/contest2026_284_board/common/src/esp_board_mmcsd.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/fs/fs.h>
#include <nuttx/mmcsd.h>
#include <nuttx/spi/spi.h>

#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#include "espressif/esp_spi.h"
#include "esp_ldo_regulator.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VELAPOKA_SD_POWERUP_DELAY_MS  100

/****************************************************************************
 * Private Data
 ****************************************************************************/

static esp_ldo_channel_handle_t g_sd_ldo;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_mmcsd_initialize(void)
{
  FAR struct spi_dev_s *spi;
  struct geometry geo;
  uint64_t capacity;
  int fd;
  int ret;
  const esp_ldo_channel_config_t ldo_cfg =
  {
    .chan_id = BOARD_VELAPOKA_SD_LDO_CHANNEL,
    .voltage_mv = BOARD_VELAPOKA_SD_LDO_MV,
    .flags.adjustable = 1,
  };

  ret = esp_configgpio(BOARD_VELAPOKA_SD_D3, OUTPUT | PULLUP);
  if (ret < 0)
    {
      return ret;
    }

  esp_gpiowrite(BOARD_VELAPOKA_SD_D3, true);

  /* DAT1 and DAT2 are unused in SPI mode, but SD cards require them to stay
   * high during entry to SPI mode.
   */

  esp_configgpio(BOARD_VELAPOKA_SD_D1, INPUT | PULLUP);
  esp_configgpio(BOARD_VELAPOKA_SD_D2, INPUT | PULLUP);
  spi = esp_spibus_initialize(ESPRESSIF_SPI2);
  if (spi == NULL)
    {
      return -ENODEV;
    }

  /* Match the official SDSPI sequence: establish inactive SPI levels while
   * the card is unpowered, then enable LDO4.  This prevents GPIO function and
   * matrix setup from presenting CS/clock glitches to a powered card.
   */

  ret = esp_ldo_acquire_channel(&ldo_cfg, &g_sd_ldo);
  if (ret != ESP_OK)
    {
      syslog(LOG_ERR, "ERROR: MicroSD LDO4 enable failed: %d\n", ret);
      return -EIO;
    }

  up_mdelay(VELAPOKA_SD_POWERUP_DELAY_MS);

  syslog(LOG_INFO, "MicroSD GPIO: CS42=%d CLK43=%d MOSI44=%d MISO39=%d\n",
         esp_gpioread(BOARD_VELAPOKA_SD_D3),
         esp_gpioread(BOARD_VELAPOKA_SD_CLK),
         esp_gpioread(BOARD_VELAPOKA_SD_CMD),
         esp_gpioread(BOARD_VELAPOKA_SD_D0));

  ret = mmcsd_spislotinitialize(0, 0, spi);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: bind SPI2 to /dev/mmcsd0 failed: %d\n",
             ret);
      return ret;
    }

  /* mmcsd_spislotinitialize() intentionally registers the block driver
   * even when media initialization fails, to support later insertion.  Open
   * the node and query its geometry so the boot log reports card readiness
   * rather than only successful driver registration.
   */

  fd = open("/dev/mmcsd0", O_RDONLY);
  if (fd < 0)
    {
      ret = -errno;
      syslog(LOG_ERR,
             "ERROR: MicroSD card initialization failed after registration: "
             "%d\n",
             ret);
      return ret;
    }

  ret = ioctl(fd, BIOC_GEOMETRY, (unsigned long)(uintptr_t)&geo);
  close(fd);
  if (ret < 0 || !geo.geo_available)
    {
      ret = ret < 0 ? -errno : -ENODEV;
      syslog(LOG_ERR,
             "ERROR: MicroSD geometry unavailable after registration: %d\n",
             ret);
      return ret;
    }

  capacity = (uint64_t)geo.geo_nsectors * geo.geo_sectorsize;
  syslog(LOG_INFO,
         "MicroSD: /dev/mmcsd0 ready, sectors=%" PRIuOFF
         " sector_size=%u capacity=%" PRIu64 " bytes (SPI2, LDO4)\n",
         geo.geo_nsectors, geo.geo_sectorsize, capacity);
  return OK;
}
