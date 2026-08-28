/****************************************************************************
 * vendor/openvela/boards/contest2026_284_board/src/velapoka_bsp.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/fs/fs.h>
#include <nuttx/i2c/i2c_master.h>

#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#include "espressif/esp_i2c_bitbang.h"
#include <arch/board/velapoka_bsp.h>

#ifdef CONFIG_ESPRESSIF_SPIRAM
#  include "esp_psram.h"
#endif

#define VELAPOKA_STATUS_PATH  "/dev/velapoka"

static uint32_t g_velapoka_ready = VELAPOKA_CAP_UART;
static FAR struct i2c_master_s *g_velapoka_i2c;

static ssize_t velapoka_status_read(FAR struct file *filep,
                                    FAR char *buffer, size_t buflen)
{
  char status[192];
  size_t psram_size = 0;
  size_t available;
  size_t count;
  int len;

#ifdef CONFIG_ESPRESSIF_SPIRAM
  if (esp_psram_is_initialized())
    {
      psram_size = esp_psram_get_size();
    }
#endif

  len = snprintf(status, sizeof(status),
                 "{\"board\":\"ESP32-P4X-Function-EV-Board\","
                 "\"product\":\"VelaPoka\",\"bsp\":1,"
                 "\"capabilities\":\"0x%08lx\","
                 "\"ready\":\"0x%08lx\","
                 "\"psram_size\":%lu,"
                 "\"lcd\":\"1024x600\",\"touch\":\"%s\"}\n",
                 (unsigned long)VELAPOKA_CAPABILITIES,
                 (unsigned long)g_velapoka_ready,
                 (unsigned long)psram_size,
#ifdef CONFIG_VELAPOKA_TOUCHSCREEN
                 CONFIG_VELAPOKA_TOUCHSCREEN_PATH
#else
                 "disabled"
#endif
                 );
  if (len < 0)
    {
      return -EIO;
    }

  if (filep->f_pos >= len)
    {
      return 0;
    }

  available = (size_t)len - filep->f_pos;
  count = buflen < available ? buflen : available;
  memcpy(buffer, status + filep->f_pos, count);
  filep->f_pos += count;
  return count;
}

static const struct file_operations g_velapoka_status_fops =
{
  NULL,                    /* open */
  NULL,                    /* close */
  velapoka_status_read,    /* read */
  NULL,                    /* write */
  NULL,                    /* seek */
  NULL,                    /* ioctl */
};

uint32_t velapoka_bsp_capabilities(void)
{
  return VELAPOKA_CAPABILITIES;
}

uint32_t velapoka_bsp_ready_mask(void)
{
  return g_velapoka_ready;
}

void velapoka_bsp_mark_ready(uint32_t mask)
{
  g_velapoka_ready |= mask & VELAPOKA_CAPABILITIES;
}

FAR struct i2c_master_s *velapoka_i2c_initialize(void)
{
  if (g_velapoka_i2c == NULL)
    {
      g_velapoka_i2c = esp_i2cbus_bitbang_initialize();
      if (g_velapoka_i2c != NULL)
        {
          velapoka_bsp_mark_ready(VELAPOKA_CAP_I2C);
        }
    }

  return g_velapoka_i2c;
}

int velapoka_lcd_backlight(bool enable)
{
  esp_gpiowrite(BOARD_VELAPOKA_LCD_BACKLIGHT, enable);
  return OK;
}

int velapoka_lcd_reset(void)
{
  esp_gpiowrite(BOARD_VELAPOKA_LCD_RESET, false);
  up_mdelay(20);
  esp_gpiowrite(BOARD_VELAPOKA_LCD_RESET, true);
  up_mdelay(120);
  return OK;
}

int velapoka_bsp_initialize(void)
{
  int ret;

#ifdef CONFIG_ESPRESSIF_SPIRAM
  if (esp_psram_is_initialized())
    {
      velapoka_bsp_mark_ready(VELAPOKA_CAP_PSRAM);
    }
#endif

  /* Keep the backlight dark until a DSI framebuffer driver is ready. */

  ret = esp_configgpio(BOARD_VELAPOKA_LCD_BACKLIGHT, OUTPUT);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_configgpio(BOARD_VELAPOKA_LCD_RESET, OUTPUT);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_configgpio(BOARD_VELAPOKA_ETH_PHY_RESET, OUTPUT);
  if (ret < 0)
    {
      return ret;
    }

  velapoka_lcd_backlight(false);
  esp_gpiowrite(BOARD_VELAPOKA_ETH_PHY_RESET, true);
  velapoka_bsp_mark_ready(VELAPOKA_CAP_CONTROL_GPIO);

  if (velapoka_i2c_initialize() == NULL)
    {
      syslog(LOG_ERR, "ERROR: VelaPoka software I2C initialization failed\n");
      return -ENODEV;
    }

  ret = register_driver(VELAPOKA_STATUS_PATH, &g_velapoka_status_fops,
                        0444, NULL);
  if (ret < 0 && ret != -EEXIST)
    {
      syslog(LOG_ERR, "ERROR: register %s failed: %d\n",
             VELAPOKA_STATUS_PATH, ret);
      return ret;
    }

#ifdef CONFIG_VELAPOKA_TOUCHSCREEN
  ret = velapoka_touchscreen_initialize();
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "WARNING: GT911 initialization failed: %d; continuing\n",
             ret);
    }
#endif

#ifdef CONFIG_VELAPOKA_CAMERA
  ret = velapoka_camera_initialize();
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "WARNING: SC2336 camera probe failed: %d; continuing\n", ret);
    }
#endif

#ifdef CONFIG_VELAPOKA_DISPLAY
  ret = velapoka_display_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: MIPI-DSI display initialization failed: %d\n",
             ret);
      return ret;
    }
#endif

  syslog(LOG_INFO,
         "VelaPoka BSP: capabilities=%08lx ready=%08lx\n",
         (unsigned long)VELAPOKA_CAPABILITIES,
         (unsigned long)g_velapoka_ready);
  return OK;
}
