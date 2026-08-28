/****************************************************************************
 * board/contest_board/src/velapoka_display.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/semaphore.h>
#include <nuttx/video/fb.h>
#include <nuttx/video/mipi_dsi.h>
#include <nuttx/video/mipi_display.h>

#include <arch/board/board.h>
#include <arch/board/velapoka_bsp.h>

#include "esp_ldo_regulator.h"
#include "espressif/esp_mipi_dsi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* LVGL renders into the inactive framebuffer and queues it for selection at
 * the next DSI VSYNC boundary.  This keeps CPU writes away from the frame
 * currently scanned by DW-GDMA.
 */

#define VELAPOKA_FB_COUNT 2

/* Keep each PSRAM cache write-back short while DW-GDMA continuously scans
 * the active video framebuffer.  LVGL can merge distant dirty objects into
 * a tall bounding rectangle; flushing that rectangle in one operation can
 * monopolize PSRAM long enough for a visible horizontal underrun.
 */

#define VELAPOKA_FB_FLUSH_ROWS 8

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

struct velapoka_fb_s
{
  struct fb_vtable_s vtable;
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  FAR uint8_t *memory;
  size_t frame_length;
#ifdef CONFIG_FB_SYNC
  sem_t vsync_sem;
#endif
  int power;
  bool active;
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
static struct velapoka_fb_s g_framebuffer;

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

static int velapoka_fb_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                    FAR struct fb_videoinfo_s *vinfo)
{
  FAR struct velapoka_fb_s *fb = (FAR struct velapoka_fb_s *)vtable;

  if (vinfo == NULL)
    {
      return -EINVAL;
    }

  memcpy(vinfo, &fb->vinfo, sizeof(*vinfo));
  return OK;
}

static int velapoka_fb_getplaneinfo(FAR struct fb_vtable_s *vtable,
                                    int planeno,
                                    FAR struct fb_planeinfo_s *pinfo)
{
  FAR struct velapoka_fb_s *fb = (FAR struct velapoka_fb_s *)vtable;

  /* LVGL's NuttX fbdev port queries display 1 to discover the second half
   * of a consecutive double framebuffer.  Both queries describe the same
   * virtual plane; display 1 is not a second hardware color plane.
   */

  if ((planeno != 0 && planeno != 1) || pinfo == NULL)
    {
      return -EINVAL;
    }

  memcpy(pinfo, &fb->pinfo, sizeof(*pinfo));
  return OK;
}

static int velapoka_fb_pandisplay(FAR struct fb_vtable_s *vtable,
                                  FAR struct fb_planeinfo_s *pinfo)
{
  FAR struct velapoka_fb_s *fb = (FAR struct velapoka_fb_s *)vtable;
  FAR uint8_t *frame;

  if (pinfo == NULL || pinfo->xoffset != 0 ||
      pinfo->yoffset >= fb->pinfo.yres_virtual ||
      pinfo->yoffset % fb->vinfo.yres != 0)
    {
      return -EINVAL;
    }

  frame = fb->memory + (size_t)pinfo->yoffset * fb->pinfo.stride;
  return esp_mipi_dsi_queue_framebuffer(frame, fb->frame_length);
}

#ifdef CONFIG_FB_UPDATE
static int velapoka_fb_updatearea(FAR struct fb_vtable_s *vtable,
                                  FAR const struct fb_area_s *area)
{
  FAR struct velapoka_fb_s *fb = (FAR struct velapoka_fb_s *)vtable;
  size_t row;
  size_t offset;
  size_t length;
  size_t rows;
  int ret;

  if (area == NULL || area->w == 0 || area->h == 0 ||
      area->x >= fb->vinfo.xres ||
      area->y >= fb->pinfo.yres_virtual ||
      area->x + area->w > fb->vinfo.xres ||
      area->y + area->h > fb->pinfo.yres_virtual)
    {
      return -EINVAL;
    }

  for (row = 0; row < area->h; row += rows)
    {
      rows = area->h - row;
      if (rows > VELAPOKA_FB_FLUSH_ROWS)
        {
          rows = VELAPOKA_FB_FLUSH_ROWS;
        }

      offset = ((size_t)area->y + row) * fb->pinfo.stride +
               (size_t)area->x * 2;
      length = (rows - 1) * fb->pinfo.stride + (size_t)area->w * 2;
      ret = esp_mipi_dsi_flush_framebuffer(fb->memory + offset, length);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}
#endif

static void velapoka_fb_vsync(FAR void *arg, bool frame_done)
{
  FAR struct velapoka_fb_s *fb = arg;
#ifdef CONFIG_FB_SYNC
  int semcount;
#endif

  if (frame_done)
    {
      fb_remove_paninfo(&fb->vtable, FB_NO_OVERLAY);
    }

  fb_notify_vsync(&fb->vtable);

#ifdef CONFIG_FB_SYNC
  nxsem_get_value(&fb->vsync_sem, &semcount);
  if (semcount < 1)
    {
      nxsem_post(&fb->vsync_sem);
    }
#endif
}

#ifdef CONFIG_FB_SYNC
static int velapoka_fb_waitforvsync(FAR struct fb_vtable_s *vtable)
{
  FAR struct velapoka_fb_s *fb = (FAR struct velapoka_fb_s *)vtable;

  /* FBIO_WAITFORVSYNC means the next boundary, not a boundary retained while
   * nobody was waiting.  Drain the binary notification before blocking.  A
   * VSYNC racing with the drain is retained by the semaphore and satisfies
   * the wait below.
   */

  while (nxsem_trywait(&fb->vsync_sem) == OK)
    {
    }

  return nxsem_wait_uninterruptible(&fb->vsync_sem);
}
#endif

static int velapoka_fb_getpower(FAR struct fb_vtable_s *vtable)
{
  FAR struct velapoka_fb_s *fb = (FAR struct velapoka_fb_s *)vtable;
  return fb->power;
}

static int velapoka_fb_setpower(FAR struct fb_vtable_s *vtable, int power)
{
  FAR struct velapoka_fb_s *fb = (FAR struct velapoka_fb_s *)vtable;
  int ret;

  if (power < 0)
    {
      return -EINVAL;
    }

  power = power != 0;
  if (power == fb->power)
    {
      return OK;
    }

  if (power == 0)
    {
      velapoka_lcd_backlight(false);
      ret = esp_mipi_dsi_video_stop();
    }
  else
    {
      ret = esp_mipi_dsi_video_start();
      if (ret >= 0)
        {
          velapoka_lcd_backlight(true);
        }
    }

  if (ret < 0)
    {
      return ret;
    }

  fb->power = power;
  return OK;
}

static int velapoka_fb_register(void)
{
  FAR struct velapoka_fb_s *fb = &g_framebuffer;
  FAR uint8_t *initial_frame;
  int ret;

  memset(fb, 0, sizeof(*fb));
  fb->vinfo.fmt = FB_FMT_RGB16_565;
  fb->vinfo.xres = BOARD_VELAPOKA_LCD_WIDTH;
  fb->vinfo.yres = BOARD_VELAPOKA_LCD_HEIGHT;
  fb->vinfo.nplanes = 1;

  fb->pinfo.stride = BOARD_VELAPOKA_LCD_WIDTH * 2;
  fb->frame_length = (size_t)fb->pinfo.stride *
                     BOARD_VELAPOKA_LCD_HEIGHT;
  fb->pinfo.fblen = fb->frame_length * VELAPOKA_FB_COUNT;
  fb->pinfo.display = 0;
  fb->pinfo.bpp = 16;
  fb->pinfo.xres_virtual = BOARD_VELAPOKA_LCD_WIDTH;
  fb->pinfo.yres_virtual = BOARD_VELAPOKA_LCD_HEIGHT *
                           VELAPOKA_FB_COUNT;
  fb->memory = memalign(64, fb->pinfo.fblen);
  if (fb->memory == NULL)
    {
      return -ENOMEM;
    }

  fb->pinfo.fbmem = fb->memory;
  fb->vtable.getvideoinfo = velapoka_fb_getvideoinfo;
  fb->vtable.getplaneinfo = velapoka_fb_getplaneinfo;
  fb->vtable.pandisplay = velapoka_fb_pandisplay;
#ifdef CONFIG_FB_UPDATE
  fb->vtable.updatearea = velapoka_fb_updatearea;
#endif
#ifdef CONFIG_FB_SYNC
  fb->vtable.waitforvsync = velapoka_fb_waitforvsync;
  nxsem_init(&fb->vsync_sem, 0, 0);
#endif
  fb->vtable.getpower = velapoka_fb_getpower;
  fb->vtable.setpower = velapoka_fb_setpower;

  memset(fb->memory, 0, fb->pinfo.fblen);
  if (VELAPOKA_FB_COUNT > 1)
    {
      memcpy(fb->memory + fb->frame_length, fb->memory, fb->frame_length);
    }

  /* LVGL starts rendering in buffer 0.  Scan the other blank buffer until
   * the first completed LVGL frame is selected at VSYNC.
   */

  initial_frame = fb->memory + fb->frame_length *
                  (VELAPOKA_FB_COUNT - 1);
  ret = esp_mipi_dsi_bind_framebuffer(initial_frame, fb->frame_length,
                                      BOARD_VELAPOKA_LCD_WIDTH,
                                      BOARD_VELAPOKA_LCD_HEIGHT, 16);
  if (ret < 0)
    {
      free(fb->memory);
      fb->memory = NULL;
      return ret;
    }

  esp_mipi_dsi_set_vsync_callback(velapoka_fb_vsync, fb);

  ret = fb_register_device(0, 0, &fb->vtable);
  if (ret < 0)
    {
      return ret;
    }

  /* fb_register_device() clears the framebuffer through the CPU cache.
   * Write that blank frame back before enabling DW-GDMA, otherwise stale
   * PSRAM cache lines can appear near the bottom of the panel before an
   * application first refreshes /dev/fb0.
   */

  ret = esp_mipi_dsi_flush_framebuffer(fb->memory, fb->pinfo.fblen);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_mipi_dsi_video_start();
  if (ret < 0)
    {
      return ret;
    }

  fb->power = 1;
  fb->active = true;
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

  /* 1354 x 636 total pixels at 26 MHz is about 30 Hz.  Keeping the panel
   * near the camera's 30 fps rate halves continuous PSRAM/DW-GDMA traffic
   * (compared with 52 MHz) and leaves bandwidth for CSI RAW10 capture.
   */

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
    .dpi_clock_freq_mhz = 26,
    .virtual_channel = 0,
    .format = MIPI_DSI_FMT_RGB565,
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
  g_panel->format = MIPI_DSI_FMT_RGB565;
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

  ret = velapoka_fb_register();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: framebuffer setup failed: %d; using bars\n",
             ret);
      ret = esp_mipi_dsi_test_pattern_start();
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: DSI test pattern start failed: %d\n", ret);
          goto errout_panel;
        }
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
  if (g_framebuffer.active)
    {
      syslog(LOG_INFO,
             "VelaPoka display: /dev/fb0 RGB565 active (%zu bytes)\n",
             g_framebuffer.pinfo.fblen);
    }
  else
    {
      syslog(LOG_INFO, "VelaPoka display: EK79007 color bars active\n");
    }

  return OK;

errout_panel:
  g_panel = NULL;
errout_ldo:
  esp_ldo_release_channel(g_dsi_ldo);
  g_dsi_ldo = NULL;
  velapoka_lcd_backlight(false);
  return ret;
}
