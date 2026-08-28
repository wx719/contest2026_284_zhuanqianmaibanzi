/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_mipi_dsi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <debug.h>
#include <string.h>
#include <math.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>
#include <nuttx/video/mipi_dsi.h>
#include <nuttx/video/mipi_display.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_clk_tree.h"
#include "esp_irq.h"
#include "esp_private/dw_gdma.h"
#include "esp_private/periph_ctrl.h"
#include "hal/mipi_dsi_hal.h"
#include "hal/mipi_dsi_ll.h"
#include "hal/mipi_dsi_types.h"
#include "hal/dw_gdma_hal.h"
#include "hal/dw_gdma_ll.h"
#include "hal/dw_gdma_types.h"
#include "hal/lcd_types.h"
#include "hal/config.h"
#include "hal/cache_ll.h"
#include "soc/clk_tree_defs.h"
#include "soc/reg_base.h"
#include "esp_private/esp_clk_tree_common.h"

#include "esp_mipi_dsi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MIPI_DSI_TIMEOUT_CLK_MHZ  10
#define MIPI_DSI_ESCAPE_CLK_MHZ   18
#define MIPI_DSI_PHY_LOCK_RETRIES 100

/* Generic cmd/payload FIFO wait (HAL uses infinite while loops). */

#define MIPI_DSI_FIFO_TIMEOUT_MS  100

#define ESP_MIPI_DSI_FB_QUEUE_DEPTH 2

#ifndef CONFIG_ESPRESSIF_MIPI_DSI_BUS
#  define CONFIG_ESPRESSIF_MIPI_DSI_BUS 0
#endif

#ifndef CONFIG_ESPRESSIF_MIPI_DSI_LANES
#  define CONFIG_ESPRESSIF_MIPI_DSI_LANES 2
#endif

#ifndef CONFIG_ESPRESSIF_MIPI_DSI_LANE_BITRATE_MBPS
#  define CONFIG_ESPRESSIF_MIPI_DSI_LANE_BITRATE_MBPS 1000
#endif

#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
#  define ESP_MIPI_DSI_NC_ADDR(a) ((uintptr_t)CACHE_LL_L2MEM_NON_CACHE_ADDR(a))
#else
#  define ESP_MIPI_DSI_NC_ADDR(a) ((uintptr_t)(a))
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Driver private state for the single MIPI-DSI host instance. */

struct esp_mipi_dsi_priv_s
{
  struct mipi_dsi_host host;         /* NuttX MIPI-DSI host (must be first) */
  mipi_dsi_hal_context_t hal;        /* Espressif MIPI-DSI HAL context */
  dw_gdma_hal_context_t dw_hal;      /* DW-GDMA HAL for FB→bridge streaming */
  dw_gdma_channel_handle_t dma_chan; /* Reserved DW-GDMA channel */
  mutex_t lock;                      /* Serialize host / video / transfer */
  spinlock_t dmalock;                /* ISR vs video_stop/start */

  FAR struct mipi_dsi_device *device;   /* Attached panel device, or NULL */
  FAR void *fb;                         /* Bound framebuffer base, or NULL */
  FAR void *pending_fb[ESP_MIPI_DSI_FB_QUEUE_DEPTH];
  dw_gdma_link_list_handle_t dma_link;  /* Internal-memory DMA link list */
  dw_gdma_lli_handle_t lli;             /* Non-cacheable LLI item */
  esp_mipi_dsi_vsync_callback_t vsync_callback;
  FAR void *vsync_arg;

  size_t fb_size;                    /* Bound framebuffer size in bytes */
  uint16_t dpi_h_res;                /* Active width from DPI config */
  uint16_t dpi_v_res;                /* Active height from DPI config */
  soc_module_clk_t phy_pllref_clk_src;
  soc_module_clk_t phy_cfg_clk_src;
  soc_module_clk_t dpi_clk_src;

  int dma_chan_id;                   /* Channel assigned by DW-GDMA core */
  int bridge_cpuint;                 /* CPU IRQ allocated for DSI bridge */
  uint8_t num_data_lanes;            /* Active data lane count */
  uint8_t fb_bpp;                    /* Bound FB bits per pixel */
  uint8_t pending_head;              /* Pending FB queue consumer */
  uint8_t pending_tail;              /* Pending FB queue producer */
  uint8_t pending_count;             /* Pending FB queue occupancy */
  float lane_bit_rate_mbps;          /* PHY lane bitrate (Mbps) */
  bool initialized;                  /* Host registered and ready */
  bool dpi_configured;               /* DPI timing programmed */
  bool fb_bound;                     /* Framebuffer bound to DW-GDMA */
  bool dma_enabled;                  /* Allow ISR to re-arm DW-GDMA */
  bool video_running;                /* Host in HS video mode */
  bool video_started_once;           /* Bridge has streamed at least once */
  bool underrun_pending;             /* Bridge needs task-context recovery */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int esp_mipi_dsi_attach(FAR struct mipi_dsi_host *host,
                               FAR struct mipi_dsi_device *device);
static int esp_mipi_dsi_detach(FAR struct mipi_dsi_host *host,
                               FAR struct mipi_dsi_device *device);
static ssize_t esp_mipi_dsi_transfer(FAR struct mipi_dsi_host *host,
                                     FAR const struct mipi_dsi_msg *msg);
static void esp_mipi_dsi_set_cmd_speed(FAR mipi_dsi_hal_context_t *hal,
                                       bool use_lpm);
static int esp_mipi_dsi_write_short(FAR mipi_dsi_hal_context_t *hal,
                                    uint8_t vc, mipi_dsi_data_type_t dt,
                                    uint16_t header);
static int esp_mipi_dsi_write_long(FAR mipi_dsi_hal_context_t *hal,
                                   uint8_t vc, mipi_dsi_data_type_t dt,
                                   FAR const uint8_t *buf, uint16_t len);
static int esp_mipi_dsi_read_short(FAR mipi_dsi_hal_context_t *hal,
                                   uint8_t vc, mipi_dsi_data_type_t dt,
                                   uint16_t header, FAR void *rx,
                                   uint16_t rx_len);
static lcd_color_format_t esp_mipi_dsi_map_format(uint8_t format);
static bool IRAM_ATTR esp_mipi_dsi_dma_callback(
  dw_gdma_channel_handle_t chan,
  FAR const dw_gdma_trans_done_event_data_t *event_data,
  FAR void *arg);
static int IRAM_ATTR esp_mipi_dsi_bridge_isr(int irq, FAR void *context,
                                             FAR void *arg);
static void IRAM_ATTR esp_mipi_dsi_dma_restart(
    FAR struct esp_mipi_dsi_priv_s *priv);
static bool IRAM_ATTR esp_mipi_dsi_switch_pending(
    FAR struct esp_mipi_dsi_priv_s *priv);
static int esp_mipi_dsi_dma_setup(FAR struct esp_mipi_dsi_priv_s *priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct mipi_dsi_host_ops g_esp_mipi_dsi_ops =
{
  .attach   = esp_mipi_dsi_attach,
  .detach   = esp_mipi_dsi_detach,
  .transfer = esp_mipi_dsi_transfer,
};

static struct esp_mipi_dsi_priv_s g_esp_mipi_dsi =
{
  .host =
    {
      .bus = CONFIG_ESPRESSIF_MIPI_DSI_BUS,
      .ops = &g_esp_mipi_dsi_ops,
    },
  .lock = NXMUTEX_INITIALIZER,
  .dmalock = SP_UNLOCKED,
  .phy_pllref_clk_src = SOC_MOD_CLK_INVALID,
  .phy_cfg_clk_src = SOC_MOD_CLK_INVALID,
  .dpi_clk_src = SOC_MOD_CLK_INVALID,
  .dma_chan_id = -1,
  .bridge_cpuint = -ENOMEM,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_mipi_dsi_dma_restart
 *
 * Description:
 *   Re-arm a single-item DW-GDMA link list so the same framebuffer keeps
 *   streaming.
 *
 * Input Parameters:
 *   priv - Driver private state
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void IRAM_ATTR esp_mipi_dsi_dma_restart(
    FAR struct esp_mipi_dsi_priv_s *priv)
{
  dw_gdma_block_markers_t markers =
  {
    .is_valid = true,
    .is_last = true,
  };

  if (priv->dma_chan == NULL || priv->dma_link == NULL ||
      priv->lli == NULL)
    {
      return;
    }

  /* Match the ESP-IDF DPI callback.  dma_link owns an internal-memory LLI. */

  dw_gdma_lli_set_block_markers(priv->lli, markers);
  dw_gdma_channel_use_link_list(priv->dma_chan, priv->dma_link);
  dw_gdma_channel_enable_ctrl(priv->dma_chan, true);
}

/****************************************************************************
 * Name: esp_mipi_dsi_switch_pending
 *
 * Description:
 *   Select the next queued framebuffer at a bridge VSYNC boundary.  The
 *   caller must hold dmalock.
 *
 * Input Parameters:
 *   priv - Driver private state
 *
 * Returned Value:
 *   true when a queued framebuffer was selected; false otherwise.
 *
 ****************************************************************************/

static bool IRAM_ATTR esp_mipi_dsi_switch_pending(
    FAR struct esp_mipi_dsi_priv_s *priv)
{
  if (!priv->dma_enabled || priv->pending_count == 0)
    {
      return false;
    }

  priv->fb = priv->pending_fb[priv->pending_head];
  priv->pending_head = (priv->pending_head + 1) %
                       ESP_MIPI_DSI_FB_QUEUE_DEPTH;
  priv->pending_count--;
  dw_gdma_ll_lli_set_src_addr(priv->lli, (uint32_t)(uintptr_t)priv->fb);
  dw_gdma_ll_lli_set_src_master_port(priv->lli, (intptr_t)priv->fb);
  return true;
}

static bool IRAM_ATTR esp_mipi_dsi_dma_callback(
  dw_gdma_channel_handle_t chan,
  FAR const dw_gdma_trans_done_event_data_t *event_data,
  FAR void *arg)
{
  FAR struct esp_mipi_dsi_priv_s *priv = arg;
  irqstate_t flags;

  UNUSED(chan);
  UNUSED(event_data);

  flags = spin_lock_irqsave(&priv->dmalock);
  if (priv->dma_enabled)
    {
      esp_mipi_dsi_dma_restart(priv);
    }

  spin_unlock_irqrestore(&priv->dmalock, flags);
  return false;
}

/****************************************************************************
 * Name: esp_mipi_dsi_bridge_isr
 *
 * Description:
 *   Switch queued framebuffers on the rev3 DSI bridge VSYNC event and
 *   notify the framebuffer upper-half.
 ****************************************************************************/

static int IRAM_ATTR esp_mipi_dsi_bridge_isr(int irq, FAR void *context,
                                             FAR void *arg)
{
  FAR struct esp_mipi_dsi_priv_s *priv = arg;
  esp_mipi_dsi_vsync_callback_t callback;
  FAR void *callback_arg;
  irqstate_t flags;
  uint32_t status;
  bool frame_done;

  UNUSED(irq);
  UNUSED(context);

  status = mipi_dsi_brg_ll_get_interrupt_status(priv->hal.bridge);
  mipi_dsi_brg_ll_clear_interrupt_status(priv->hal.bridge, status);
  if ((status & MIPI_DSI_BRG_LL_EVENT_UNDERRUN) != 0)
    {
      /* An underrun is level-like while the bridge remains starved.  Leaving
       * it enabled creates an interrupt storm that can starve UART and the
       * camera task.  Defer bridge recovery to the next framebuffer queue.
       */

      mipi_dsi_brg_ll_enable_interrupt(priv->hal.bridge,
                                       MIPI_DSI_BRG_LL_EVENT_UNDERRUN,
                                       false);
      priv->underrun_pending = true;
    }

  if ((status & MIPI_DSI_BRG_LL_EVENT_VSYNC) == 0)
    {
      return OK;
    }

  flags = spin_lock_irqsave(&priv->dmalock);
  frame_done = esp_mipi_dsi_switch_pending(priv);
  callback = priv->vsync_callback;
  callback_arg = priv->vsync_arg;
  spin_unlock_irqrestore(&priv->dmalock, flags);

  if (callback != NULL)
    {
      callback(callback_arg, frame_done);
    }

  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_dma_setup
 *
 * Description:
 *   Reserve and configure one DW-GDMA channel for MEM→DSI streaming.
 *
 * Input Parameters:
 *   priv - Driver private state
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int esp_mipi_dsi_dma_setup(FAR struct esp_mipi_dsi_priv_s *priv)
{
  dw_gdma_channel_alloc_config_t config =
  {
    .src =
    {
      .block_transfer_type = DW_GDMA_BLOCK_TRANSFER_LIST,
      .role = DW_GDMA_ROLE_MEM,
      .handshake_type = DW_GDMA_HANDSHAKE_HW,
      .num_outstanding_requests = 5,
    },
    .dst =
    {
      .block_transfer_type = DW_GDMA_BLOCK_TRANSFER_LIST,
      .role = DW_GDMA_ROLE_PERIPH_DSI,
      .handshake_type = DW_GDMA_HANDSHAKE_HW,
      .num_outstanding_requests = 2,
    },
    .flow_controller = DW_GDMA_FLOW_CTRL_SELF,
    .chan_priority = 1,
  };
  dw_gdma_event_callbacks_t callbacks =
  {
    .on_full_trans_done = esp_mipi_dsi_dma_callback,
  };
  FAR dw_gdma_dev_t *dev;
  int ret;

  if (priv->dma_chan != NULL)
    {
      return OK;
    }

  ret = dw_gdma_new_channel(&config, &priv->dma_chan);
  if (ret != ESP_OK)
    {
      verr("esp_mipi_dsi: DW-GDMA channel allocation failed: %d\n", ret);
      return -EIO;
    }

  ret = dw_gdma_channel_get_id(priv->dma_chan, &priv->dma_chan_id);
  if (ret != ESP_OK)
    {
      dw_gdma_del_channel(priv->dma_chan);
      priv->dma_chan = NULL;
      return -EIO;
    }

  priv->dw_hal.dev = DW_GDMA_LL_GET_HW(0);
  dev = priv->dw_hal.dev;

  dw_gdma_ll_channel_set_trans_flow(dev, priv->dma_chan_id,
                                    DW_GDMA_ROLE_MEM,
                                    DW_GDMA_ROLE_PERIPH_DSI,
                                    DW_GDMA_FLOW_CTRL_SELF);
  dw_gdma_ll_channel_set_src_multi_block_type(dev, priv->dma_chan_id,
                                              DW_GDMA_BLOCK_TRANSFER_LIST);
  dw_gdma_ll_channel_set_dst_multi_block_type(dev, priv->dma_chan_id,
                                              DW_GDMA_BLOCK_TRANSFER_LIST);
  dw_gdma_ll_channel_set_src_handshake_interface(dev, priv->dma_chan_id,
                                                 DW_GDMA_HANDSHAKE_HW);
  dw_gdma_ll_channel_set_dst_handshake_interface(dev, priv->dma_chan_id,
                                                 DW_GDMA_HANDSHAKE_HW);
  dw_gdma_ll_channel_set_dst_handshake_periph(dev, priv->dma_chan_id,
                                              DW_GDMA_ROLE_PERIPH_DSI);
  dw_gdma_ll_channel_set_priority(dev, priv->dma_chan_id, 1);
  dw_gdma_ll_channel_set_src_outstanding_limit(dev, priv->dma_chan_id,
                                               5);
  dw_gdma_ll_channel_set_dst_outstanding_limit(dev, priv->dma_chan_id,
                                               2);
  ret = dw_gdma_channel_register_event_callbacks(priv->dma_chan,
                                                  &callbacks, priv);
  if (ret != ESP_OK)
    {
      dw_gdma_del_channel(priv->dma_chan);
      priv->dma_chan = NULL;
      priv->dw_hal.dev = NULL;
      priv->dma_chan_id = -1;
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_map_format
 *
 * Description:
 *   Map a NuttX MIPI-DSI pixel format to the Espressif LCD color format
 *   used by the DPI host and bridge.
 *
 * Input Parameters:
 *   format - MIPI_DSI_FMT_* value from the panel/board DPI config
 *
 * Returned Value:
 *   Corresponding lcd_color_format_t (defaults to RGB565).
 *
 ****************************************************************************/

static lcd_color_format_t esp_mipi_dsi_map_format(uint8_t format)
{
  switch (format)
    {
      case MIPI_DSI_FMT_RGB888:
      case MIPI_DSI_FMT_RGB666:
      case MIPI_DSI_FMT_RGB666_PACKED:
        return LCD_COLOR_FMT_RGB888;

      case MIPI_DSI_FMT_RGB565:
      default:
        return LCD_COLOR_FMT_RGB565;
    }
}

/****************************************************************************
 * Name: esp_mipi_dsi_set_cmd_speed
 *
 * Description:
 *   Configure generic/DCS packet speed (LP vs HS) for command-mode
 *   transfers.
 *
 * Input Parameters:
 *   hal     - MIPI-DSI HAL context
 *   use_lpm - true for low-power mode; false for high-speed
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void esp_mipi_dsi_set_cmd_speed(FAR mipi_dsi_hal_context_t *hal,
                                       bool use_lpm)
{
  mipi_dsi_ll_trans_speed_mode_t speed =
    use_lpm ? MIPI_DSI_LL_TRANS_SPEED_LP : MIPI_DSI_LL_TRANS_SPEED_HS;

  mipi_dsi_host_ll_set_gen_short_wr_speed_mode(hal->host, 0, speed);
  mipi_dsi_host_ll_set_gen_short_wr_speed_mode(hal->host, 1, speed);
  mipi_dsi_host_ll_set_gen_short_wr_speed_mode(hal->host, 2, speed);
  mipi_dsi_host_ll_set_gen_long_wr_speed_mode(hal->host, speed);
  mipi_dsi_host_ll_set_gen_short_rd_speed_mode(hal->host, 0, speed);
  mipi_dsi_host_ll_set_gen_short_rd_speed_mode(hal->host, 1, speed);
  mipi_dsi_host_ll_set_gen_short_rd_speed_mode(hal->host, 2, speed);
  mipi_dsi_host_ll_set_dcs_short_wr_speed_mode(hal->host, 0, speed);
  mipi_dsi_host_ll_set_dcs_short_wr_speed_mode(hal->host, 1, speed);
  mipi_dsi_host_ll_set_dcs_long_wr_speed_mode(hal->host, speed);
  mipi_dsi_host_ll_set_dcs_short_rd_speed_mode(hal->host, 0, speed);
  mipi_dsi_host_ll_set_mrps_speed_mode(hal->host, speed);
}

/****************************************************************************
 * Name: esp_mipi_dsi_attach
 *
 * Description:
 *   Attach a MIPI-DSI device (panel) to this host. Updates the PHY data
 *   lane count if the device requests a different lane configuration.
 *
 * Input Parameters:
 *   host   - MIPI-DSI host instance
 *   device - Device to attach (must not be NULL)
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure
 *   (-EINVAL, -EBUSY, or lock failure).
 *
 ****************************************************************************/

static int esp_mipi_dsi_attach(FAR struct mipi_dsi_host *host,
                               FAR struct mipi_dsi_device *device)
{
  FAR struct esp_mipi_dsi_priv_s *priv =
    (FAR struct esp_mipi_dsi_priv_s *)host;
  int ret;

  if (device == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->device != NULL)
    {
      nxmutex_unlock(&priv->lock);
      return -EBUSY;
    }

  if (device->lanes == 0 || device->lanes > MIPI_DSI_LL_MAX_DATA_LANES)
    {
      nxmutex_unlock(&priv->lock);
      return -EINVAL;
    }

  priv->device = device;
  device->host = host;

  /* Prefer device lane count when attached (panel will set this in Phase 3).
   * PHY lane number was already programmed at host init from Kconfig.
   */

  if (device->lanes != priv->num_data_lanes)
    {
      mipi_dsi_phy_ll_set_data_lane_number(priv->hal.host, device->lanes);
      priv->num_data_lanes = device->lanes;
    }

  nxmutex_unlock(&priv->lock);

  vinfo("esp_mipi_dsi: attached device '%s' lanes=%u fmt=%u flags=0x%lx\n",
        device->name, device->lanes, device->format,
        (unsigned long)device->mode_flags);
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_detach
 *
 * Description:
 *   Detach a previously attached MIPI-DSI device from this host.
 *
 * Input Parameters:
 *   host   - MIPI-DSI host instance
 *   device - Device to detach (must match the currently attached device)
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int esp_mipi_dsi_detach(FAR struct mipi_dsi_host *host,
                               FAR struct mipi_dsi_device *device)
{
  FAR struct esp_mipi_dsi_priv_s *priv =
    (FAR struct esp_mipi_dsi_priv_s *)host;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->device == NULL || priv->device != device)
    {
      nxmutex_unlock(&priv->lock);
      return -EINVAL;
    }

  priv->device = NULL;
  device->host = NULL;
  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_wait_cmd_not_full
 *
 * Description:
 *   Poll until the generic command FIFO has space for a new header, or
 *   until MIPI_DSI_FIFO_TIMEOUT_MS elapses.
 *
 * Input Parameters:
 *   hal  - MIPI-DSI HAL context
 *   what - Context string used in the timeout error log
 *
 * Returned Value:
 *   Zero (OK) on success; -ETIMEDOUT if the FIFO stays full.
 *
 ****************************************************************************/

static int esp_mipi_dsi_wait_cmd_not_full(FAR mipi_dsi_hal_context_t *hal,
                                          FAR const char *what)
{
  int i;

  for (i = 0; i < MIPI_DSI_FIFO_TIMEOUT_MS; i++)
    {
      if (!mipi_dsi_host_ll_gen_is_cmd_fifo_full(hal->host))
        {
          return OK;
        }

      up_mdelay(1);
    }

  syslog(LOG_ERR, "ERROR: DSI FIFO timeout: %s\n", what);
  syslog(LOG_ERR, "ERROR: DSI CMD_PKT_STATUS=%08lx\n",
         (unsigned long)hal->host->cmd_pkt_status.val);
  syslog(LOG_ERR, "ERROR: DSI PWR_UP=%08lx\n",
         (unsigned long)hal->host->pwr_up.val);
  syslog(LOG_ERR, "ERROR: DSI MODE_CFG=%08lx\n",
         (unsigned long)hal->host->mode_cfg.val);
  syslog(LOG_ERR, "ERROR: DSI PHY_STATUS=%08lx\n",
         (unsigned long)hal->host->phy_status.val);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: esp_mipi_dsi_wait_pld_not_full
 *
 * Description:
 *   Poll until the generic write payload FIFO has space, or until
 *   MIPI_DSI_FIFO_TIMEOUT_MS elapses.
 *
 * Input Parameters:
 *   hal  - MIPI-DSI HAL context
 *   what - Context string used in the timeout error log
 *
 * Returned Value:
 *   Zero (OK) on success; -ETIMEDOUT if the FIFO stays full.
 *
 ****************************************************************************/

static int esp_mipi_dsi_wait_pld_not_full(FAR mipi_dsi_hal_context_t *hal,
                                          FAR const char *what)
{
  int i;

  for (i = 0; i < MIPI_DSI_FIFO_TIMEOUT_MS; i++)
    {
      if (!mipi_dsi_host_ll_gen_is_write_fifo_full(hal->host))
        {
          return OK;
        }

      up_mdelay(1);
    }

  verr("esp_mipi_dsi: %s timeout (%d ms)\n", what,
       MIPI_DSI_FIFO_TIMEOUT_MS);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: esp_mipi_dsi_write_short
 *
 * Description:
 *   Transmit a short DCS/generic packet by writing the packet header to
 *   the generic command FIFO and waiting for it to drain.
 *
 * Input Parameters:
 *   hal    - MIPI-DSI HAL context
 *   vc     - Virtual channel (0..3)
 *   dt     - MIPI-DSI data type
 *   header - Two-byte short-packet payload / parameters
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int esp_mipi_dsi_write_short(FAR mipi_dsi_hal_context_t *hal,
                                    uint8_t vc, mipi_dsi_data_type_t dt,
                                    uint16_t header)
{
  uint8_t msb = (header >> 8) & 0xff;
  uint8_t lsb = header & 0xff;
  int ret;

  ret = esp_mipi_dsi_wait_cmd_not_full(hal, "cmd FIFO full (short)");
  if (ret < 0)
    {
      return ret;
    }

  mipi_dsi_host_ll_gen_set_packet_header(hal->host, vc, dt, msb, lsb);
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_write_long
 *
 * Description:
 *   Transmit a long DCS/generic packet: push the payload into the write
 *   FIFO in 32-bit words, then write the packet header with the length.
 *
 * Input Parameters:
 *   hal - MIPI-DSI HAL context
 *   vc  - Virtual channel (0..3)
 *   dt  - MIPI-DSI data type
 *   buf - Payload bytes (may be NULL only when len is 0)
 *   len - Payload length in bytes
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int esp_mipi_dsi_write_long(FAR mipi_dsi_hal_context_t *hal,
                                   uint8_t vc, mipi_dsi_data_type_t dt,
                                   FAR const uint8_t *buf, uint16_t len)
{
  FAR const uint8_t *payload = buf;
  uint32_t remain = len;
  uint32_t temp;
  int ret;

  while (remain >= 4)
    {
      memcpy(&temp, payload, sizeof(temp));
      ret = esp_mipi_dsi_wait_pld_not_full(hal,
                                           "payload FIFO full (long)");
      if (ret < 0)
        {
          return ret;
        }

      mipi_dsi_host_ll_gen_write_payload_fifo(hal->host, temp);
      payload += 4;
      remain -= 4;
    }

  if (remain > 0)
    {
      temp = 0;
      memcpy(&temp, payload, remain);
      ret = esp_mipi_dsi_wait_pld_not_full(hal,
                                           "payload FIFO full (long tail)");
      if (ret < 0)
        {
          return ret;
        }

      mipi_dsi_host_ll_gen_write_payload_fifo(hal->host, temp);
    }

  ret = esp_mipi_dsi_wait_cmd_not_full(hal, "cmd FIFO full (long)");
  if (ret < 0)
    {
      return ret;
    }

  mipi_dsi_host_ll_gen_set_packet_header(hal->host, vc, dt,
                                         (len >> 8) & 0xff, len & 0xff);
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_read_short
 *
 * Description:
 *   Perform a short-packet read: set the maximum return packet size,
 *   enable BTA, issue the read request, and copy response bytes from the
 *   read FIFO into rx.
 *
 * Input Parameters:
 *   hal    - MIPI-DSI HAL context
 *   vc     - Virtual channel (0..3)
 *   dt     - MIPI-DSI read data type
 *   header - Two-byte short-packet parameters for the read request
 *   rx     - Buffer to receive response bytes
 *   rx_len - Size of rx in bytes
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int esp_mipi_dsi_read_short(FAR mipi_dsi_hal_context_t *hal,
                                   uint8_t vc, mipi_dsi_data_type_t dt,
                                   uint16_t header, FAR void *rx,
                                   uint16_t rx_len)
{
  FAR uint8_t *receive = rx;
  uint32_t temp;
  uint32_t counter = 0;
  int i;
  int ret;

  ret = esp_mipi_dsi_write_short(hal, vc, MIPI_DSI_DT_SET_MAXIMUM_RETURN_PKT,
                                 rx_len);
  if (ret < 0)
    {
      return ret;
    }

  mipi_dsi_host_ll_enable_video_mode(hal->host, false);
  mipi_dsi_host_ll_enable_bta(hal->host, true);
  mipi_dsi_host_ll_gen_set_rx_vcid(hal->host, vc);

  ret = esp_mipi_dsi_write_short(hal, vc, dt, header);
  if (ret < 0)
    {
      goto errout_bta;
    }

  for (i = 0; i < MIPI_DSI_FIFO_TIMEOUT_MS; i++)
    {
      if (!mipi_dsi_host_ll_gen_is_read_cmd_busy(hal->host))
        {
          break;
        }

      up_mdelay(1);
    }

  if (i >= MIPI_DSI_FIFO_TIMEOUT_MS)
    {
      verr("esp_mipi_dsi: read cmd busy timeout\n");
      ret = -ETIMEDOUT;
      goto errout_bta;
    }

  for (i = 0; i < MIPI_DSI_FIFO_TIMEOUT_MS; i++)
    {
      if (!mipi_dsi_host_ll_gen_is_read_fifo_empty(hal->host))
        {
          break;
        }

      up_mdelay(1);
    }

  if (i >= MIPI_DSI_FIFO_TIMEOUT_MS)
    {
      verr("esp_mipi_dsi: read FIFO empty timeout\n");
      ret = -ETIMEDOUT;
      goto errout_bta;
    }

  while (!mipi_dsi_host_ll_gen_is_read_fifo_empty(hal->host))
    {
      temp = mipi_dsi_host_ll_gen_read_payload_fifo(hal->host);
      for (i = 0; i < 4; i++)
        {
          if (counter < rx_len)
            {
              receive[counter++] = (temp >> (8 * i)) & 0xff;
            }
        }
    }

  ret = OK;

errout_bta:
  mipi_dsi_host_ll_enable_bta(hal->host, false);
  return ret;
}

/****************************************************************************
 * Name: esp_mipi_dsi_transfer
 *
 * Description:
 *   LP/HS command-mode transfer for DCS and generic short/long packets.
 *   Reads temporarily leave video mode (BTA); writes may run during HS
 *   video blanking when the host is already streaming.
 *
 * Input Parameters:
 *   host - MIPI-DSI host instance
 *   msg  - Transfer descriptor (type, channel, buffers, flags)
 *
 * Returned Value:
 *   Number of bytes transferred on success (tx_len or rx_len); a
 *   negated errno value on failure.
 *
 ****************************************************************************/

static ssize_t esp_mipi_dsi_transfer(FAR struct mipi_dsi_host *host,
                                     FAR const struct mipi_dsi_msg *msg)
{
  FAR struct esp_mipi_dsi_priv_s *priv =
    (FAR struct esp_mipi_dsi_priv_s *)host;
  FAR mipi_dsi_hal_context_t *hal = &priv->hal;
  FAR const uint8_t *tx = msg->tx_buf;
  bool was_video;
  bool is_read;
  bool use_lpm;
  uint16_t header;
  int ret;

  if (msg == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return -EAGAIN;
    }

  /* Keep HS video running for LP command writes (lp_cmd_en in
   * blanking). Only tear down video for reads (need BTA / cmd mode).
   */

  was_video = priv->video_running;
  is_read = (msg->rx_len > 0 && msg->rx_buf != NULL);

  if (was_video && is_read)
    {
      mipi_dsi_brg_ll_enable_dpi_output(hal->bridge, false);
      mipi_dsi_host_ll_enable_video_mode(hal->host, false);
    }
  else if (!was_video)
    {
      mipi_dsi_host_ll_enable_video_mode(hal->host, false);
    }

  use_lpm = (msg->flags & MIPI_DSI_MSG_USE_LPM) != 0;
  esp_mipi_dsi_set_cmd_speed(hal, use_lpm);

  /* Only request ACK when the message asks for it.
   * An unanswered BTA blocks the generic cmd FIFO.
   */

  mipi_dsi_host_ll_enable_cmd_ack(hal->host,
                                (msg->flags & MIPI_DSI_MSG_REQ_ACK) != 0);

  if (mipi_dsi_packet_format_is_short(msg->type))
    {
      header = 0;
      if (msg->tx_len >= 1 && tx != NULL)
        {
          header |= tx[0];
        }

      if (msg->tx_len >= 2 && tx != NULL)
        {
          header |= ((uint16_t)tx[1]) << 8;
        }

      if (is_read)
        {
          ret = esp_mipi_dsi_read_short(hal, msg->channel,
                                        (mipi_dsi_data_type_t)msg->type,
                                        header, msg->rx_buf,
                                        (uint16_t)msg->rx_len);
        }
      else
        {
          ret = esp_mipi_dsi_write_short(hal, msg->channel,
                                          (mipi_dsi_data_type_t)msg->type,
                                          header);
        }
    }
  else if (mipi_dsi_packet_format_is_long(msg->type))
    {
      if (msg->tx_len > 0 && tx == NULL)
        {
          nxmutex_unlock(&priv->lock);
          return -EINVAL;
        }

      ret = esp_mipi_dsi_write_long(hal, msg->channel,
                                    (mipi_dsi_data_type_t)msg->type,
                                    tx, (uint16_t)msg->tx_len);
    }
  else
    {
      nxmutex_unlock(&priv->lock);
      return -ENOTSUP;
    }

  if (was_video && is_read)
    {
      mipi_dsi_host_ll_enable_video_mode(hal->host, true);
      mipi_dsi_brg_ll_enable_dpi_output(hal->bridge, true);
      mipi_dsi_brg_ll_update_dpi_config(hal->bridge);
    }

  nxmutex_unlock(&priv->lock);

  if (ret < 0)
    {
      return ret;
    }

  return is_read ? (ssize_t)msg->rx_len : (ssize_t)msg->tx_len;
}

/****************************************************************************
 * Name: esp_mipi_dsi_wait_phy_ready
 *
 * Description:
 *   Wait for the MIPI PHY PLL to lock and for all configured data lanes
 *   to enter stop state after bring-up.
 *
 * Input Parameters:
 *   priv - Driver private state
 *
 * Returned Value:
 *   Zero (OK) on success; -ETIMEDOUT if PLL lock or lane stop-state fails.
 *
 ****************************************************************************/

static int esp_mipi_dsi_wait_phy_ready(FAR struct esp_mipi_dsi_priv_s *priv)
{
  int i;

  for (i = 0; i < MIPI_DSI_PHY_LOCK_RETRIES; i++)
    {
      if (mipi_dsi_phy_ll_is_pll_locked(priv->hal.host))
        {
          break;
        }

      up_mdelay(1);
    }

  if (i >= MIPI_DSI_PHY_LOCK_RETRIES)
    {
      syslog(LOG_ERR, "ERROR: esp_mipi_dsi: PHY PLL lock timeout\n");
      return -ETIMEDOUT;
    }

  for (i = 0; i < MIPI_DSI_PHY_LOCK_RETRIES; i++)
    {
      if (mipi_dsi_phy_ll_are_lanes_stopped(priv->hal.host,
                                            priv->num_data_lanes))
        {
          return OK;
        }

      up_mdelay(1);
    }

  syslog(LOG_ERR, "ERROR: esp_mipi_dsi: PHY lanes stop-state timeout\n");
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: esp_mipi_dsi_hw_init
 *
 * Description:
 *   Clock/reset enable and HAL/PHY bring-up (ESP-IDF esp_lcd_new_dsi_bus
 *   sequence adapted for NuttX). Leaves the host in command mode with LP
 *   transfers enabled.
 *
 * Input Parameters:
 *   priv - Driver private state (lanes and bitrate must already be set)
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int esp_mipi_dsi_hw_init(FAR struct esp_mipi_dsi_priv_s *priv)
{
  mipi_dsi_hal_config_t hal_config;
  mipi_dsi_phy_pllref_clock_source_t phy_clk_src;
  uint32_t phy_clk_src_freq_hz = 0;
  esp_err_t err;
  int bus_id = priv->host.bus;
  int ret;

  if (priv->num_data_lanes == 0 ||
      priv->num_data_lanes > MIPI_DSI_LL_MAX_DATA_LANES)
    {
      return -EINVAL;
    }

  if (priv->lane_bit_rate_mbps < MIPI_DSI_LL_MIN_PHY_MBPS ||
      priv->lane_bit_rate_mbps > MIPI_DSI_LL_MAX_PHY_MBPS)
    {
      return -EINVAL;
    }

  PERIPH_RCC_ATOMIC()
    {
      mipi_dsi_ll_enable_bus_clock(bus_id, true);
      mipi_dsi_ll_reset_register(bus_id);
    }

#ifdef CONFIG_ESP32P4_SELECTS_REV_LESS_V3
  phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY;
#else
  phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT;
#endif

  err = esp_clk_tree_enable_src((soc_module_clk_t)phy_clk_src, true);
  if (err != ESP_OK)
    {
      verr("esp_mipi_dsi: enable PHY PLL ref clk failed: %d\n",
           (int)err);
      return -EIO;
    }

  priv->phy_pllref_clk_src = (soc_module_clk_t)phy_clk_src;

  err = esp_clk_tree_enable_src(
          (soc_module_clk_t)MIPI_DSI_PHY_CFG_CLK_SRC_DEFAULT, true);
  if (err != ESP_OK)
    {
      verr("esp_mipi_dsi: enable PHY cfg clk failed: %d\n",
           (int)err);
      return -EIO;
    }

  priv->phy_cfg_clk_src =
    (soc_module_clk_t)MIPI_DSI_PHY_CFG_CLK_SRC_DEFAULT;

  PERIPH_RCC_ATOMIC()
    {
      mipi_dsi_ll_set_phy_config_clock_source(bus_id,
                                          MIPI_DSI_PHY_CFG_CLK_SRC_DEFAULT);
      mipi_dsi_ll_enable_phy_config_clock(bus_id, true);
      mipi_dsi_ll_set_phy_pllref_clock_source(bus_id, phy_clk_src);
      mipi_dsi_ll_set_phy_pll_ref_clock_div(bus_id, 1);
      mipi_dsi_ll_enable_phy_pllref_clock(bus_id, true);
    }

  memset(&hal_config, 0, sizeof(hal_config));
  hal_config.bus_id = bus_id;
  hal_config.lane_bit_rate_mbps = priv->lane_bit_rate_mbps;
  hal_config.num_data_lanes = priv->num_data_lanes;
  mipi_dsi_hal_init(&priv->hal, &hal_config);

  err = esp_clk_tree_src_get_freq_hz(phy_clk_src,
                                     ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                                     &phy_clk_src_freq_hz);
  if (err != ESP_OK || phy_clk_src_freq_hz == 0)
    {
      verr("esp_mipi_dsi: get PHY clk freq failed: %d\n",
           (int)err);
      return -EIO;
    }

  mipi_dsi_hal_configure_phy_pll(&priv->hal, phy_clk_src_freq_hz,
                                 priv->lane_bit_rate_mbps);

  ret = esp_mipi_dsi_wait_phy_ready(priv);
  if (ret < 0)
    {
      return ret;
    }

  mipi_dsi_host_ll_enable_video_mode(priv->hal.host, false);
  mipi_dsi_host_ll_set_clock_lane_state(priv->hal.host,
                                        MIPI_DSI_LL_CLOCK_LANE_STATE_AUTO);
  mipi_dsi_phy_ll_set_switch_time(priv->hal.host, 50, 104, 46, 128);

  mipi_dsi_host_ll_enable_rx_crc(priv->hal.host, true);
  mipi_dsi_host_ll_enable_rx_ecc(priv->hal.host, true);
  mipi_dsi_host_ll_enable_tx_eotp(priv->hal.host, true, false);

  mipi_dsi_host_ll_set_timeout_clock_division(
    priv->hal.host,
    (uint32_t)roundf(priv->lane_bit_rate_mbps / 8.0f /
                     MIPI_DSI_TIMEOUT_CLK_MHZ));
  mipi_dsi_host_ll_set_escape_clock_division(
    priv->hal.host,
    (uint32_t)roundf(priv->lane_bit_rate_mbps / 8.0f /
                     MIPI_DSI_ESCAPE_CLK_MHZ));
  mipi_dsi_host_ll_set_timeout_count(priv->hal.host, 0, 0, 0, 0, 0, 0, 0);
  mipi_dsi_phy_ll_set_max_read_time(priv->hal.host, 6000);
  mipi_dsi_phy_ll_set_stop_wait_time(priv->hal.host, 0x3f);

  mipi_dsi_host_ll_enable_te_ack(priv->hal.host, false);

  /* Keep ACK off unless a transfer sets MIPI_DSI_MSG_REQ_ACK.  An
   * unanswered BTA holds the generic cmd FIFO and looks like a boot hang.
   */

  mipi_dsi_host_ll_enable_cmd_ack(priv->hal.host, false);
  esp_mipi_dsi_set_cmd_speed(&priv->hal, true);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_mipi_dsi_initialize
 *
 * Description:
 *   Enable clocks/resets for the MIPI-DSI host, PHY and bridge; configure
 *   the PHY PLL for the selected lane bitrate; leave the controller in
 *   command mode for LP DCS/generic transfers; and register the host via
 *   mipi_dsi_host_register().
 *
 *   Does not power VDD_MIPI_DPHY — board code must enable the MIPI PHY
 *   supply (e.g. on-chip LDO) before calling this.
 *
 * Input Parameters:
 *   cfg - Optional bus config (lanes / bitrate). NULL uses Kconfig defaults.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp_mipi_dsi_initialize(
      FAR const struct esp_mipi_dsi_bus_config_s *cfg)
{
  FAR struct esp_mipi_dsi_priv_s *priv = &g_esp_mipi_dsi;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  /* Board may override Kconfig; zero / NULL means use defaults. */

  if (cfg != NULL && cfg->num_data_lanes != 0)
    {
      priv->num_data_lanes = cfg->num_data_lanes;
    }
  else
    {
      priv->num_data_lanes = CONFIG_ESPRESSIF_MIPI_DSI_LANES;
    }

  if (cfg != NULL && cfg->lane_bit_rate_mbps != 0)
    {
      priv->lane_bit_rate_mbps = (float)cfg->lane_bit_rate_mbps;
    }
  else
    {
      priv->lane_bit_rate_mbps =
        (float)CONFIG_ESPRESSIF_MIPI_DSI_LANE_BITRATE_MBPS;
    }

  ret = esp_mipi_dsi_hw_init(priv);
  if (ret < 0)
    {
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  ret = mipi_dsi_host_register(&priv->host);
  if (ret < 0)
    {
      nxmutex_unlock(&priv->lock);
      verr("esp_mipi_dsi: host register failed: %d\n", ret);
      return ret;
    }

  /* Only mark ready after registration succeeds so host_get() / retries
   * do not observe a half-initialized host.
   */

  priv->initialized = true;
  nxmutex_unlock(&priv->lock);

  vinfo("esp_mipi_dsi: host registered (bus=%d, lanes=%u, "
         "%u Mbps/lane)\n",
         priv->host.bus, priv->num_data_lanes,
         (unsigned int)(priv->lane_bit_rate_mbps + 0.5f));
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_host_get
 *
 * Description:
 *   Return the registered Espressif MIPI-DSI host, or NULL if not
 *   initialized.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Pointer to the registered mipi_dsi_host on success; NULL if
 *   esp_mipi_dsi_initialize() has not completed successfully.
 *
 ****************************************************************************/

FAR struct mipi_dsi_host *esp_mipi_dsi_host_get(void)
{
  if (!g_esp_mipi_dsi.initialized)
    {
      return NULL;
    }

  return &g_esp_mipi_dsi.host;
}

/****************************************************************************
 * Name: esp_mipi_dsi_configure_dpi
 *
 * Description:
 *   Configure DPI clock divider and host/bridge video timing for later
 *   HS video mode. Does not start streaming and does not bind a
 *   framebuffer.
 *
 * Input Parameters:
 *   cfg - DPI timing / format (must not be NULL; board/panel supplies this)
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp_mipi_dsi_configure_dpi(
      FAR const struct esp_mipi_dsi_dpi_config_s *cfg)
{
  FAR struct esp_mipi_dsi_priv_s *priv = &g_esp_mipi_dsi;
  FAR mipi_dsi_hal_context_t *hal;
  mipi_dsi_dpi_clock_source_t dpi_clk_src;
  lcd_color_format_t color_fmt;
  uint32_t dpi_clk_src_freq_hz = 0;
  uint32_t dpi_div;
  float dpi_clk_src_mhz;
  esp_err_t err;
  int bus_id;
  int ret;

  if (cfg == NULL || cfg->h_res == 0 || cfg->v_res == 0 ||
      cfg->dpi_clock_freq_mhz == 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return -EAGAIN;
    }

  hal = &priv->hal;
  bus_id = priv->host.bus;
  dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  color_fmt = esp_mipi_dsi_map_format(cfg->format);

  err = esp_clk_tree_src_get_freq_hz(dpi_clk_src,
                                     ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                                     &dpi_clk_src_freq_hz);
  if (err != ESP_OK || dpi_clk_src_freq_hz == 0)
    {
      nxmutex_unlock(&priv->lock);
      return -EIO;
    }

  dpi_clk_src_mhz = (float)dpi_clk_src_freq_hz / 1000000.0f;
  dpi_div = mipi_dsi_hal_host_dpi_calculate_divider(
              hal, dpi_clk_src_mhz, (float)cfg->dpi_clock_freq_mhz);

  err = esp_clk_tree_enable_src((soc_module_clk_t)dpi_clk_src, true);
  if (err != ESP_OK)
    {
      nxmutex_unlock(&priv->lock);
      return -EIO;
    }

  priv->dpi_clk_src = (soc_module_clk_t)dpi_clk_src;

  PERIPH_RCC_ATOMIC()
    {
      mipi_dsi_ll_set_dpi_clock_source(bus_id, dpi_clk_src);
      mipi_dsi_ll_set_dpi_clock_div(bus_id, dpi_div);
      mipi_dsi_ll_enable_dpi_clock(bus_id, true);
    }

  mipi_dsi_host_ll_dpi_set_vcid(hal->host, cfg->virtual_channel);
  mipi_dsi_host_ll_dpi_set_color_coding(hal->host, color_fmt, 0);
  mipi_dsi_host_ll_dpi_set_timing_polarity(hal->host, false, false, false,
                                           false, false);

  /* Match ESP-IDF dpi panel defaults. */

  mipi_dsi_host_ll_dpi_enable_lp_horizontal_timing(hal->host, true, true);
  mipi_dsi_host_ll_dpi_enable_lp_vertical_timing(hal->host, true, true,
                                                 true, true);
  mipi_dsi_host_ll_dpi_enable_lp_command(hal->host, true);
  mipi_dsi_host_ll_dpi_enable_frame_ack(hal->host, true);

  mipi_dsi_host_ll_dpi_set_video_burst_type(
    hal->host, MIPI_DSI_LL_VIDEO_BURST_WITH_SYNC_PULSES);
  mipi_dsi_host_ll_dpi_set_video_packet_pixel_num(hal->host, cfg->h_res);
  mipi_dsi_host_ll_dpi_set_trunks_num(hal->host, 0);
  mipi_dsi_host_ll_dpi_set_null_packet_size(hal->host, 0);

  mipi_dsi_hal_host_dpi_set_horizontal_timing(hal, cfg->hsync_pulse_width,
                                              cfg->hsync_back_porch,
                                              cfg->h_res,
                                              cfg->hsync_front_porch);
  mipi_dsi_hal_host_dpi_set_vertical_timing(hal, cfg->vsync_pulse_width,
                                            cfg->vsync_back_porch,
                                            cfg->v_res,
                                            cfg->vsync_front_porch);

  /* Bridge color/timing; DW-GDMA bind fills pixel_bits / flow later. */

  mipi_dsi_brg_ll_set_input_color_format(hal->bridge, color_fmt);
  mipi_dsi_brg_ll_set_output_color_format(hal->bridge, color_fmt, 0);
  mipi_dsi_brg_ll_set_underrun_discard_count(hal->bridge, cfg->h_res);
  mipi_dsi_brg_ll_enable(hal->bridge, true);
  mipi_dsi_brg_ll_update_dpi_config(hal->bridge);

  priv->dpi_h_res = cfg->h_res;
  priv->dpi_v_res = cfg->v_res;
  priv->dpi_configured = true;
  nxmutex_unlock(&priv->lock);

  vinfo("esp_mipi_dsi: DPI configured %ux%u @ %lu MHz\n",
         cfg->h_res, cfg->v_res, (unsigned long)cfg->dpi_clock_freq_mhz);

  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_bind_framebuffer
 *
 * Description:
 *   Bind a RGB framebuffer in memory to the DSI bridge via DW-GDMA so that
 *   esp_mipi_dsi_video_start() can stream pixels continuously.  Call after
 *   esp_mipi_dsi_configure_dpi().  fb must remain valid while video runs;
 *   after CPU writes, callers should call
 *   esp_mipi_dsi_flush_framebuffer() on the dirty region before expecting
 *   the panel to show new pixels.
 *
 * Input Parameters:
 *   fb      - Framebuffer base (DMA-capable, typically PSRAM)
 *   fb_size - Size in bytes (h_res * v_res * bpp / 8)
 *   h_res   - Active width in pixels
 *   v_res   - Active height in lines
 *   bpp     - Bits per pixel (16 for RGB565, 24 for RGB888)
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp_mipi_dsi_bind_framebuffer(FAR void *fb, size_t fb_size,
                                  uint16_t h_res, uint16_t v_res,
                                  uint8_t bpp)
{
  FAR struct esp_mipi_dsi_priv_s *priv = &g_esp_mipi_dsi;
  dw_gdma_block_transfer_config_t transfer =
  {
    .src =
    {
      .burst_mode = DW_GDMA_BURST_MODE_INCREMENT,
      .burst_items = DW_GDMA_BURST_ITEMS_512,
      .burst_len = 16,
      .width = DW_GDMA_TRANS_WIDTH_64,
    },
    .dst =
    {
      .addr = MIPI_DSI_BRG_MEM_BASE,
      .burst_mode = DW_GDMA_BURST_MODE_FIXED,
      .burst_items = DW_GDMA_BURST_ITEMS_256,
      .burst_len = 16,
      .width = DW_GDMA_TRANS_WIDTH_64,
    },
  };
  dw_gdma_block_markers_t markers =
  {
    .is_valid = true,
    .is_last = true,
  };
  dw_gdma_link_list_config_t link_config =
  {
    .num_items = 1,
    .link_type = DW_GDMA_LINKED_LIST_TYPE_SINGLY,
  };
  size_t expect;
  int ret;

  if (fb == NULL || fb_size == 0 || h_res == 0 || v_res == 0 ||
      (bpp != 16 && bpp != 24))
    {
      return -EINVAL;
    }

  expect = (size_t)h_res * (size_t)v_res * ((size_t)bpp / 8);
  if (fb_size < expect)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized || !priv->dpi_configured)
    {
      nxmutex_unlock(&priv->lock);
      return -EAGAIN;
    }

  if (priv->video_running)
    {
      nxmutex_unlock(&priv->lock);
      return -EBUSY;
    }

  ret = esp_mipi_dsi_dma_setup(priv);
  if (ret < 0)
    {
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  if (priv->dma_link == NULL)
    {
      ret = dw_gdma_new_link_list(&link_config, &priv->dma_link);
      if (ret != ESP_OK)
        {
          nxmutex_unlock(&priv->lock);
          return -ENOMEM;
        }

      priv->lli = dw_gdma_link_list_get_item(priv->dma_link, 0);
      if (priv->lli == NULL)
        {
          dw_gdma_del_link_list(priv->dma_link);
          priv->dma_link = NULL;
          nxmutex_unlock(&priv->lock);
          return -EIO;
        }
    }

  transfer.src.addr = (uint32_t)(uintptr_t)fb;
  transfer.size = fb_size * 8 / 64;
  if (dw_gdma_lli_config_transfer(priv->lli, &transfer) != ESP_OK ||
      dw_gdma_lli_set_block_markers(priv->lli, markers) != ESP_OK)
    {
      nxmutex_unlock(&priv->lock);
      return -EIO;
    }

  esp_cache_msync(fb, fb_size,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                  ESP_CACHE_MSYNC_FLAG_UNALIGNED);

  /* Bridge expects DW-GDMA as flow controller. */

  mipi_dsi_brg_ll_set_num_pixel_bits(priv->hal.bridge,
                                     (uint32_t)h_res * v_res * bpp);
  mipi_dsi_brg_ll_set_flow_controller(priv->hal.bridge,
                                      MIPI_DSI_LL_FLOW_CONTROLLER_DMA);
  mipi_dsi_brg_ll_set_multi_block_number(priv->hal.bridge, 1);
  mipi_dsi_brg_ll_set_burst_len(priv->hal.bridge, 256);
  mipi_dsi_brg_ll_set_empty_threshold(priv->hal.bridge, 1024 - 256);
  mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);

  priv->fb = fb;
  priv->fb_size = fb_size;
  priv->fb_bpp = bpp;
  priv->fb_bound = true;
  nxmutex_unlock(&priv->lock);

  vinfo("esp_mipi_dsi: FB bound %p %ux%u %u bpp (%zu bytes) via DW-GDMA\n",
        fb, h_res, v_res, bpp, fb_size);

  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_flush_framebuffer
 *
 * Description:
 *   Cache write-back (C2M) so DW-GDMA sees CPU stores to the framebuffer.
 *   Call after CPU writes to a bound FB (full or partial region).
 *
 * Input Parameters:
 *   addr - Start of the dirty region (typically within the bound FB)
 *   len  - Length in bytes
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp_mipi_dsi_flush_framebuffer(FAR void *addr, size_t len)
{
  esp_err_t err;

  if (addr == NULL || len == 0)
    {
      return -EINVAL;
    }

  err = esp_cache_msync(addr, len,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                        ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  return (err == ESP_OK) ? OK : -EIO;
}

/****************************************************************************
 * Name: esp_mipi_dsi_queue_framebuffer
 ****************************************************************************/

int esp_mipi_dsi_queue_framebuffer(FAR void *fb, size_t fb_size)
{
  FAR struct esp_mipi_dsi_priv_s *priv = &g_esp_mipi_dsi;
  irqstate_t flags;
  int ret;

  if (fb == NULL || fb_size == 0)
    {
      return -EINVAL;
    }

  ret = esp_mipi_dsi_flush_framebuffer(fb, fb_size);
  if (ret < 0)
    {
      return ret;
    }

  flags = spin_lock_irqsave(&priv->dmalock);
  if (!priv->fb_bound || fb_size != priv->fb_size)
    {
      ret = -EINVAL;
    }
  else if (!priv->dma_enabled)
    {
      /* A stopped stream can switch buffers immediately.  This lets camera
       * preview finish CPU rendering and cache write-back before DSI starts
       * reading PSRAM again.
       */

      priv->fb = fb;
      dw_gdma_ll_lli_set_src_addr(priv->lli,
                                  (uint32_t)(uintptr_t)priv->fb);
      dw_gdma_ll_lli_set_src_master_port(priv->lli,
                                         (intptr_t)priv->fb);
      priv->pending_head = 0;
      priv->pending_tail = 0;
      priv->pending_count = 0;
      ret = OK;
    }
  else if (priv->pending_count >= ESP_MIPI_DSI_FB_QUEUE_DEPTH)
    {
      ret = -EBUSY;
    }
  else
    {
      if (priv->underrun_pending)
        {
          /* The hardware cannot recover an already-blue DPI stream merely
           * by clearing the status bit.  Restart the bridge at a complete
           * framebuffer boundary, then re-arm one-shot underrun detection.
           */

          mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, false);
          mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);
          mipi_dsi_brg_ll_clear_interrupt_status(
            priv->hal.bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN);
          mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, true);
          mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);
          priv->underrun_pending = false;
        }

      priv->pending_fb[priv->pending_tail] = fb;
      priv->pending_tail = (priv->pending_tail + 1) %
                           ESP_MIPI_DSI_FB_QUEUE_DEPTH;
      priv->pending_count++;
      ret = OK;
    }

  spin_unlock_irqrestore(&priv->dmalock, flags);
  return ret;
}

/****************************************************************************
 * Name: esp_mipi_dsi_set_vsync_callback
 ****************************************************************************/

int esp_mipi_dsi_set_vsync_callback(esp_mipi_dsi_vsync_callback_t callback,
                                    FAR void *arg)
{
  FAR struct esp_mipi_dsi_priv_s *priv = &g_esp_mipi_dsi;
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->dmalock);
  priv->vsync_callback = callback;
  priv->vsync_arg = arg;
  spin_unlock_irqrestore(&priv->dmalock, flags);
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_start
 *
 * Description:
 *   Switch the host to video mode and enable bridge DPI output.  If a
 *   framebuffer was bound, also starts DW-GDMA streaming.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp_mipi_dsi_video_start(void)
{
  FAR struct esp_mipi_dsi_priv_s *priv = &g_esp_mipi_dsi;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return -EAGAIN;
    }

  if (!priv->dpi_configured)
    {
      nxmutex_unlock(&priv->lock);
      verr("esp_mipi_dsi: call esp_mipi_dsi_configure_dpi() first\n");
      return -EINVAL;
    }

  if (priv->bridge_cpuint < 0)
    {
      ret = esp_setup_irq(DSI_BRIDGE_INTR_SOURCE,
                          ESP_IRQ_PRIORITY_DEFAULT,
                          ESP_IRQ_TRIGGER_LEVEL);
      if (ret < 0)
        {
          nxmutex_unlock(&priv->lock);
          return ret;
        }

      priv->bridge_cpuint = ret;
      ret = irq_attach(ESP_IRQ_DSI_BRIDGE, esp_mipi_dsi_bridge_isr, priv);
      if (ret < 0)
        {
          esp_teardown_irq(DSI_BRIDGE_INTR_SOURCE,
                           priv->bridge_cpuint);
          priv->bridge_cpuint = -ENOMEM;
          nxmutex_unlock(&priv->lock);
          return ret;
        }

      up_enable_irq(ESP_IRQ_DSI_BRIDGE);
    }

  if (priv->fb_bound)
    {
      irqstate_t flags = spin_lock_irqsave(&priv->dmalock);

      /* A snapshot capture releases the DSI DW-GDMA channel in
       * esp_mipi_dsi_video_stop().  Re-acquire it here before re-arming the
       * persistent link-list item.  CSI and DSI share this DMA group, so
       * keeping an idle DSI channel allocated is not sufficient isolation
       * for a CSI capture. */

      if (priv->dma_chan == NULL)
        {
          spin_unlock_irqrestore(&priv->dmalock, flags);
          ret = esp_mipi_dsi_dma_setup(priv);
          if (ret < 0)
            {
              nxmutex_unlock(&priv->lock);
              return ret;
            }

          flags = spin_lock_irqsave(&priv->dmalock);
        }

      if (priv->video_started_once)
        {
          /* Reset the bridge FIFO/state machine after the pattern-generator
           * interval.  Clearing the underrun status alone does not recover
           * the rev3 bridge once it has latched its blue fallback output.
           */

          mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, false);
          mipi_dsi_brg_ll_enable(priv->hal.bridge, false);
          mipi_dsi_brg_ll_clear_interrupt_status(priv->hal.bridge,
                                                 UINT32_MAX);
          mipi_dsi_brg_ll_enable(priv->hal.bridge, true);
          mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);
        }

      priv->dma_enabled = true;
      esp_mipi_dsi_dma_restart(priv);
      spin_unlock_irqrestore(&priv->dmalock, flags);
    }

  mipi_dsi_host_ll_enable_video_mode(priv->hal.host, true);
  mipi_dsi_host_ll_set_clock_lane_state(
    priv->hal.host, MIPI_DSI_LL_CLOCK_LANE_STATE_AUTO);
  mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, true);
  mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);
  mipi_dsi_host_ll_dpi_set_pattern_type(priv->hal.host,
                                        MIPI_DSI_PATTERN_NONE);
  /* Do not enable the diagnostic underrun interrupt.  On ESP32-P4 rev3 the
   * source can remain asserted after the bridge has gone blue, causing a
   * level-triggered interrupt storm that starves all tasks.  VSYNC and the
   * DW-GDMA completion interrupt provide the required runtime events.
   */

  mipi_dsi_brg_ll_enable_interrupt(priv->hal.bridge,
                                   MIPI_DSI_BRG_LL_EVENT_UNDERRUN, false);
  mipi_dsi_brg_ll_clear_interrupt_status(priv->hal.bridge,
                                         MIPI_DSI_BRG_LL_EVENT_UNDERRUN);
  mipi_dsi_brg_ll_clear_interrupt_status(priv->hal.bridge,
                                         MIPI_DSI_BRG_LL_EVENT_VSYNC);
  mipi_dsi_brg_ll_enable_interrupt(priv->hal.bridge,
                                   MIPI_DSI_BRG_LL_EVENT_VSYNC, true);
  priv->video_running = true;
  priv->video_started_once = true;
  nxmutex_unlock(&priv->lock);

  vinfo("esp_mipi_dsi: video mode started%s\n",
        priv->fb_bound ? " (DW-GDMA streaming)" : " (no FB bound)");
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_test_pattern_start
 ****************************************************************************/

int esp_mipi_dsi_test_pattern_start(void)
{
  FAR struct esp_mipi_dsi_priv_s *priv = &g_esp_mipi_dsi;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized || !priv->dpi_configured)
    {
      nxmutex_unlock(&priv->lock);
      return -EAGAIN;
    }

  mipi_dsi_brg_ll_enable_interrupt(priv->hal.bridge,
                                   MIPI_DSI_BRG_LL_EVENT_VSYNC, false);
  mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, false);
  mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);
  mipi_dsi_host_ll_dpi_set_pattern_type(priv->hal.host,
                                        MIPI_DSI_PATTERN_BAR_VERTICAL);
  mipi_dsi_host_ll_enable_video_mode(priv->hal.host, true);
  priv->video_running = true;
  nxmutex_unlock(&priv->lock);

  vinfo("esp_mipi_dsi: vertical color-bar pattern started\n");
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_stop
 *
 * Description:
 *   Disable bridge DPI output and return the host to command mode for
 *   further LP transfers.  Stops DW-GDMA if running.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp_mipi_dsi_video_stop(void)
{
  FAR struct esp_mipi_dsi_priv_s *priv = &g_esp_mipi_dsi;
  irqstate_t flags;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return -EAGAIN;
    }

  /* Keep valid video packets on the DSI link while PSRAM DMA is paused.
   * The host pattern generator does not consume PSRAM and prevents the
   * EK79007/bridge from entering the unrecoverable blue underrun state.
   */

  mipi_dsi_brg_ll_enable_interrupt(priv->hal.bridge,
                                   MIPI_DSI_BRG_LL_EVENT_VSYNC, false);
  mipi_dsi_host_ll_dpi_set_pattern_type(priv->hal.host,
                                        MIPI_DSI_PATTERN_BAR_VERTICAL);
  mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, false);
  mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);

  /* Drop the enable flag before touching HW so a concurrent TFR_DONE
   * ISR cannot re-arm the channel after we disable it.
   */

  flags = spin_lock_irqsave(&priv->dmalock);
  priv->dma_enabled = false;
  if (priv->dw_hal.dev != NULL)
    {
      dw_gdma_ll_channel_enable(priv->dw_hal.dev, priv->dma_chan_id,
                                false);
      dw_gdma_ll_channel_clear_intr(priv->dw_hal.dev,
                                    priv->dma_chan_id, UINT32_MAX);
    }

  spin_unlock_irqrestore(&priv->dmalock, flags);

  /* Do not merely disable the channel for a camera snapshot.  Its shared
   * interrupt registration and DMA state remain live until it is deleted,
   * which can block the CSI peripheral-to-memory channel even though DPI
   * pixel transfers are stopped.  The framebuffer link-list is retained and
   * video_start recreates this channel before display resume. */

  if (priv->dma_chan != NULL)
    {
      ret = dw_gdma_del_channel(priv->dma_chan);
      if (ret != ESP_OK)
        {
          nxmutex_unlock(&priv->lock);
          return -EIO;
        }

      priv->dma_chan = NULL;
      priv->dma_chan_id = -1;
      priv->dw_hal.dev = NULL;
    }

  /* Keep the DSI host and PHY in HS video mode.  Leaving video mode here
   * makes the EK79007 lose link synchronization and it does not reliably
   * recover on the next start (the visible symptom is a solid pink panel).
   * Pausing the pixel DMA and DPI bridge output is sufficient to release
   * PSRAM bandwidth for CSI.
   */

  priv->video_running = false;
  nxmutex_unlock(&priv->lock);
  return OK;
}
