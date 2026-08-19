/****************************************************************************
 * vendor/openvela/boards/contest2026_284_board/src/velapoka_display.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/video/mipi_dsi.h>
#include <nuttx/video/mipi_display.h>

#include <arch/board/board.h>
#include <arch/board/velapoka_bsp.h>

#include "esp_ldo_regulator.h"
#include "espressif/esp_mipi_dsi.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct velapoka_lcd_cmd_s
{
  uint8_t cmd;
  uint8_t data;
  uint16_t delay_ms;
  bool has_data;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct velapoka_lcd_cmd_s g_ek79007_init[] =
{
  {0xb2, 0x10,   0, true},
  {0x80, 0x8b,   0, true},
  {0x81, 0x78,   0, true},
  {0x82, 0x84,   0, true},
  {0x83, 0x88,   0, true},
  {0x84, 0xa8,   0, true},
  {0x85, 0xe3,   0, true},
  {0x86, 0x88,   0, true},
  {MIPI_DCS_EXIT_SLEEP_MODE, 0, 120, false},
};

static esp_ldo_channel_handle_t g_dsi_ldo;
static FAR struct mipi_dsi_device *g_panel;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int velapoka_ek79007_send_init(void)
{
  size_t i;
  int ret;

  for (i = 0; i < sizeof(g_ek79007_init) / sizeof(g_ek79007_init[0]); i++)
    {
      FAR const struct velapoka_lcd_cmd_s *entry = &g_ek79007_init[i];

      ret = mipi_dsi_dcs_write(g_panel, entry->cmd,
                               entry->has_data ? &entry->data : NULL,
                               entry->has_data ? 1 : 0);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: EK79007 command 0x%02x failed: %d\n",
                 entry->cmd, ret);
          return ret;
        }

      if (entry->delay_ms != 0)
        {
          up_mdelay(entry->delay_ms);
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velapoka_display_initialize(void)
{
  const esp_ldo_channel_config_t ldo_cfg =
  {
    .chan_id = BOARD_VELAPOKA_DSI_LDO_CHANNEL,
    .voltage_mv = BOARD_VELAPOKA_DSI_LDO_MV,
  };

  const struct esp_mipi_dsi_bus_config_s bus_cfg =
  {
    .num_data_lanes = BOARD_VELAPOKA_LCD_DSI_LANES,
    .lane_bit_rate_mbps = BOARD_VELAPOKA_LCD_DSI_MBPS,
  };

  const struct esp_mipi_dsi_dpi_config_s dpi_cfg =
  {
    .h_res = BOARD_VELAPOKA_LCD_WIDTH,
    .v_res = BOARD_VELAPOKA_LCD_HEIGHT,
    .hsync_pulse_width = 10,
    .hsync_back_porch = 160,
    .hsync_front_porch = 160,
    .vsync_pulse_width = 1,
    .vsync_back_porch = 23,
    .vsync_front_porch = 12,
    .dpi_clock_freq_mhz = 52,
    .virtual_channel = 0,
    .format = MIPI_DSI_FMT_RGB888,
  };

  FAR struct mipi_dsi_host *host;
  int ret;

  if (g_panel != NULL)
    {
      return OK;
    }

  ret = esp_ldo_acquire_channel(&ldo_cfg, &g_dsi_ldo);
  if (ret != ESP_OK)
    {
      syslog(LOG_ERR, "ERROR: display DSI LDO acquisition failed: %d\n",
             ret);
      return -EIO;
    }

  ret = velapoka_lcd_reset();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: display reset failed: %d\n", ret);
      goto errout_ldo;
    }

  ret = esp_mipi_dsi_initialize(&bus_cfg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: DSI host initialization failed: %d\n", ret);
      goto errout_ldo;
    }

  ret = esp_mipi_dsi_configure_dpi(&dpi_cfg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: DSI DPI configuration failed: %d\n", ret);
      goto errout_ldo;
    }

  host = esp_mipi_dsi_host_get();
  if (host == NULL)
    {
      ret = -ENODEV;
      goto errout_ldo;
    }

  g_panel = mipi_dsi_device_register(host, "ek79007", 0);
  if (g_panel == NULL)
    {
      ret = -ENOMEM;
      goto errout_ldo;
    }

  g_panel->lanes = BOARD_VELAPOKA_LCD_DSI_LANES;
  g_panel->format = MIPI_DSI_FMT_RGB888;
  g_panel->mode_flags = MIPI_DSI_MODE_VIDEO |
                        MIPI_DSI_MODE_VIDEO_BURST |
                        MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
                        MIPI_DSI_MODE_LPM;

  ret = mipi_dsi_attach(g_panel);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: DSI panel attach failed: %d\n", ret);
      goto errout_panel;
    }

  ret = velapoka_ek79007_send_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: EK79007 initialization failed: %d\n", ret);
      goto errout_panel;
    }

  ret = esp_mipi_dsi_test_pattern_start();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: DSI test pattern start failed: %d\n", ret);
      goto errout_panel;
    }

  ret = mipi_dsi_dcs_set_display_on(g_panel);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: EK79007 display-on failed: %d\n", ret);
      goto errout_panel;
    }

  up_mdelay(20);
  velapoka_lcd_backlight(true);
  velapoka_bsp_mark_ready(VELAPOKA_CAP_MIPI_DSI);
  syslog(LOG_INFO, "VelaPoka display: EK79007 color bars active\n");
  return OK;

errout_panel:
  g_panel = NULL;
errout_ldo:
  esp_ldo_release_channel(g_dsi_ldo);
  g_dsi_ldo = NULL;
  velapoka_lcd_backlight(false);
  return ret;
}
