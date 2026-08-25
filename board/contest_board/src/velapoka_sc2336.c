/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/velapoka_sc2336.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/videoio.h>
#include <unistd.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/signal.h>
#include <nuttx/video/imgdata.h>
#include <nuttx/video/imgsensor.h>
#include <nuttx/video/v4l2_cap.h>
#include <nuttx/wqueue.h>

#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_ldo_regulator.h"
#include "driver/isp_core.h"
#include "hal/mipi_csi_brg_ll.h"
#include "hal/mipi_csi_host_ll.h"

#include <arch/board/board.h>
#include <arch/board/velapoka_bsp.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SC2336_REG_CHIP_ID_H  0x3107
#define SC2336_REG_CHIP_ID_L  0x3108
#define SC2336_REG_STREAM      0x0100
#define SC2336_FRAME_SIZE      (BOARD_VELAPOKA_CAMERA_WIDTH * \
                                BOARD_VELAPOKA_CAMERA_HEIGHT * 5 / 4)
#define SC2336_BUFFER_ALIGN    64

struct sc2336_reg_s
{
  uint16_t reg;
  uint8_t value;
};

#include "velapoka_sc2336_720p30.h"

static FAR struct i2c_master_s *g_sc2336_i2c;
static esp_ldo_channel_handle_t g_csi_ldo;

struct sc2336_dev_s
{
  struct imgsensor_s sensor;
  struct imgdata_s data;
  esp_cam_ctlr_handle_t csi;
  isp_proc_handle_t isp;
  struct work_s frame_work;
  FAR uint8_t *scratch;
  atomic_uintptr_t next_buffer;
  FAR imgdata_capture_t capture;
  FAR void *capture_arg;
  uint32_t completed_size;
  atomic_uint buffer_requests;
  atomic_uint frames_finished;
  bool receiving;
};

static struct sc2336_dev_s g_sc2336;

static const struct v4l2_fmtdesc g_sc2336_fmtdesc[] =
{
  {
    .index       = 0,
    .type        = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .description = "SC2336 BGGR10 packed",
    .pixelformat = V4L2_PIX_FMT_SBGGR10P,
  }
};

static const struct v4l2_frmsizeenum g_sc2336_frmsize[] =
{
  {
    .index        = 0,
    .pixel_format = V4L2_PIX_FMT_SBGGR10P,
    .type         = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete     =
      {
        .width  = BOARD_VELAPOKA_CAMERA_WIDTH,
        .height = BOARD_VELAPOKA_CAMERA_HEIGHT,
      },
  }
};

static const struct v4l2_frmivalenum g_sc2336_frmival[] =
{
  {
    .index        = 0,
    .pixel_format = V4L2_PIX_FMT_SBGGR10P,
    .width        = BOARD_VELAPOKA_CAMERA_WIDTH,
    .height       = BOARD_VELAPOKA_CAMERA_HEIGHT,
    .type         = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete     =
      {
        .numerator   = 1,
        .denominator = BOARD_VELAPOKA_CAMERA_FPS,
      },
  }
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int sc2336_readreg(FAR struct i2c_master_s *i2c, uint16_t reg,
                          FAR uint8_t *value)
{
  struct i2c_msg_s msg[2];
  uint8_t regbuf[2];

  regbuf[0] = reg >> 8;
  regbuf[1] = reg & 0xff;

  msg[0].frequency = BOARD_VELAPOKA_CAMERA_FREQUENCY;
  msg[0].addr      = BOARD_VELAPOKA_CAMERA_ADDR;
  msg[0].flags     = I2C_M_NOSTOP;
  msg[0].buffer    = regbuf;
  msg[0].length    = sizeof(regbuf);

  msg[1].frequency = BOARD_VELAPOKA_CAMERA_FREQUENCY;
  msg[1].addr      = BOARD_VELAPOKA_CAMERA_ADDR;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = value;
  msg[1].length    = 1;

  return I2C_TRANSFER(i2c, msg, 2);
}

static int sc2336_writereg(FAR struct i2c_master_s *i2c, uint16_t reg,
                           uint8_t value)
{
  struct i2c_msg_s msg;
  uint8_t buffer[3];

  buffer[0] = reg >> 8;
  buffer[1] = reg & 0xff;
  buffer[2] = value;

  msg.frequency = BOARD_VELAPOKA_CAMERA_FREQUENCY;
  msg.addr      = BOARD_VELAPOKA_CAMERA_ADDR;
  msg.flags     = 0;
  msg.buffer    = buffer;
  msg.length    = sizeof(buffer);

  return I2C_TRANSFER(i2c, &msg, 1);
}

static int sc2336_write_mode(FAR struct i2c_master_s *i2c)
{
  unsigned int i;
  int ret;

  for (i = 0; i < sizeof(g_sc2336_720p30_regs) /
                  sizeof(g_sc2336_720p30_regs[0]); i++)
    {
      ret = sc2336_writereg(i2c, g_sc2336_720p30_regs[i].reg,
                            g_sc2336_720p30_regs[i].value);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "ERROR: SC2336 register 0x%04x write failed: %d\n",
                 g_sc2336_720p30_regs[i].reg, ret);
          return ret;
        }

      if (g_sc2336_720p30_regs[i].reg == 0x0103)
        {
          nxsig_usleep(5000);
        }
    }

  return OK;
}

static bool sc2336_sensor_available(FAR struct imgsensor_s *sensor)
{
  return g_sc2336_i2c != NULL;
}

static int sc2336_sensor_noop(FAR struct imgsensor_s *sensor)
{
  return OK;
}

static FAR const char *
sc2336_sensor_name(FAR struct imgsensor_s *sensor)
{
  return "sc2336";
}

static int sc2336_validate_sensor(FAR struct imgsensor_s *sensor,
                                  imgsensor_stream_type_t type,
                                  uint8_t nr_fmt,
                                  FAR imgsensor_format_t *fmt,
                                  FAR imgsensor_interval_t *interval)
{
  if (type != IMGSENSOR_STREAM_TYPE_VIDEO || nr_fmt != 1 ||
      fmt[0].width != BOARD_VELAPOKA_CAMERA_WIDTH ||
      fmt[0].height != BOARD_VELAPOKA_CAMERA_HEIGHT ||
      fmt[0].pixelformat != IMGSENSOR_PIX_FMT_SBGGR10P ||
      interval->numerator != 1 ||
      interval->denominator != BOARD_VELAPOKA_CAMERA_FPS)
    {
      return -ENOTSUP;
    }

  return OK;
}

static int sc2336_sensor_start(FAR struct imgsensor_s *sensor,
                               imgsensor_stream_type_t type,
                               uint8_t nr_fmt,
                               FAR imgsensor_format_t *fmt,
                               FAR imgsensor_interval_t *interval)
{
  return velapoka_camera_set_stream(true);
}

static int sc2336_sensor_stop(FAR struct imgsensor_s *sensor,
                              imgsensor_stream_type_t type)
{
  return velapoka_camera_set_stream(false);
}

static const struct imgsensor_ops_s g_sc2336_sensor_ops =
{
  .is_available          = sc2336_sensor_available,
  .init                  = sc2336_sensor_noop,
  .uninit                = sc2336_sensor_noop,
  .get_driver_name       = sc2336_sensor_name,
  .validate_frame_setting = sc2336_validate_sensor,
  .start_capture         = sc2336_sensor_start,
  .stop_capture          = sc2336_sensor_stop,
};

static void sc2336_frame_worker(FAR void *arg)
{
  FAR struct sc2336_dev_s *priv = arg;
  struct timeval timestamp;

  if (priv->capture != NULL)
    {
      gettimeofday(&timestamp, NULL);
      priv->capture(0, priv->completed_size, &timestamp,
                    priv->capture_arg);
      velapoka_bsp_mark_ready(VELAPOKA_CAP_MIPI_CSI);
    }
}

static bool sc2336_get_new_buffer(esp_cam_ctlr_handle_t handle,
                                  FAR esp_cam_ctlr_trans_t *trans,
                                  FAR void *arg)
{
  FAR struct sc2336_dev_s *priv = arg;
  uintptr_t buffer;

  atomic_fetch_add_explicit(&priv->buffer_requests, 1,
                            memory_order_relaxed);
  buffer = atomic_exchange_explicit(&priv->next_buffer, 0,
                                    memory_order_acq_rel);
  trans->buffer = buffer != 0 ? (FAR void *)buffer : priv->scratch;
  trans->buflen = SC2336_FRAME_SIZE;
  return false;
}

static bool sc2336_frame_finished(esp_cam_ctlr_handle_t handle,
                                  FAR esp_cam_ctlr_trans_t *trans,
                                  FAR void *arg)
{
  FAR struct sc2336_dev_s *priv = arg;

  atomic_fetch_add_explicit(&priv->frames_finished, 1,
                            memory_order_relaxed);
  if (trans->buffer != priv->scratch && priv->capture != NULL &&
      work_available(&priv->frame_work))
    {
      priv->completed_size = trans->received_size;
      work_queue(HPWORK, &priv->frame_work, sc2336_frame_worker,
                 priv, 0);
    }

  return false;
}

static void sc2336_dump_csi_status(void)
{
  uint32_t frame_error;

  frame_error = MIPI_CSI_HOST.int_st_bndry_frame_fatal.val |
                MIPI_CSI_HOST.int_st_seq_frame_fatal.val |
                MIPI_CSI_HOST.int_st_crc_frame_fatal.val |
                MIPI_CSI_HOST.int_st_pld_crc_fatal.val;
  syslog(LOG_INFO,
         "SC2336 CSI host: main=%08lx stop=%08lx phy=%08lx "
         "phy_fatal=%08lx pkt_fatal=%08lx frame_error=%08lx\n",
         (unsigned long)MIPI_CSI_HOST.int_st_main.val,
         (unsigned long)MIPI_CSI_HOST.phy_stopstate.val,
         (unsigned long)MIPI_CSI_HOST.int_st_phy.val,
         (unsigned long)MIPI_CSI_HOST.int_st_phy_fatal.val,
         (unsigned long)MIPI_CSI_HOST.int_st_pkt_fatal.val,
         (unsigned long)frame_error);
  syslog(LOG_INFO,
         "SC2336 CSI bridge: enable=%08lx raw=%08lx depth=%lu "
         "frame_cfg=%08lx data_type=%08lx\n",
         (unsigned long)MIPI_CSI_BRIDGE.csi_en.val,
         (unsigned long)MIPI_CSI_BRIDGE.int_raw.val,
         (unsigned long)MIPI_CSI_BRIDGE.buf_flow_ctl.csi_buf_depth,
         (unsigned long)MIPI_CSI_BRIDGE.frame_cfg.val,
         (unsigned long)MIPI_CSI_BRIDGE.data_type_cfg.val);
  syslog(LOG_INFO,
         "SC2336 CSI PHY: shutdown=%08lx reset=%08lx rx=%08lx "
         "cal=%08lx test0=%08lx test1=%08lx\n",
         (unsigned long)MIPI_CSI_HOST.phy_shutdownz.val,
         (unsigned long)MIPI_CSI_HOST.dphy_rstz.val,
         (unsigned long)MIPI_CSI_HOST.phy_rx.val,
         (unsigned long)MIPI_CSI_HOST.phy_cal.val,
         (unsigned long)MIPI_CSI_HOST.phy_test_ctrl0.val,
         (unsigned long)MIPI_CSI_HOST.phy_test_ctrl1.val);
}

static int sc2336_data_init(FAR struct imgdata_s *data)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)data;
  const esp_isp_processor_cfg_t isp_config =
  {
    .clk_hz                 = 80 * 1000 * 1000,
    .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
    .input_data_color_type  = ISP_COLOR_RAW10,
    .output_data_color_type = ISP_COLOR_RAW10,
    .has_line_start_packet  = false,
    .has_line_end_packet    = false,
    .h_res                  = BOARD_VELAPOKA_CAMERA_WIDTH,
    .v_res                  = BOARD_VELAPOKA_CAMERA_HEIGHT,
    .bayer_order            = COLOR_RAW_ELEMENT_ORDER_BGGR,
    .flags.bypass_isp       = true,
  };
  esp_cam_ctlr_csi_config_t config =
  {
    .ctlr_id                = 0,
    .h_res                  = BOARD_VELAPOKA_CAMERA_WIDTH,
    .v_res                  = BOARD_VELAPOKA_CAMERA_HEIGHT,
    .data_lane_num          = BOARD_VELAPOKA_CAMERA_CSI_LANES,
    .lane_bit_rate_mbps     = 405,
    .input_data_color_type  = CAM_CTLR_COLOR_RAW10,
    .output_data_color_type = CAM_CTLR_COLOR_RAW10,
    .queue_items            = 1,
    .bk_buffer_dis          = true,
  };
  esp_cam_ctlr_evt_cbs_t callbacks =
  {
    .on_get_new_trans  = sc2336_get_new_buffer,
    .on_trans_finished = sc2336_frame_finished,
  };
  int ret;

  if (priv->csi != NULL)
    {
      return OK;
    }

  priv->scratch = memalign(SC2336_BUFFER_ALIGN, SC2336_FRAME_SIZE);
  if (priv->scratch == NULL)
    {
      return -ENOMEM;
    }

  ret = esp_cam_new_csi_ctlr(&config, &priv->csi);
  if (ret != ESP_OK)
    {
      free(priv->scratch);
      priv->scratch = NULL;
      return -EIO;
    }

  ret = esp_cam_ctlr_register_event_callbacks(priv->csi, &callbacks, priv);
  if (ret == ESP_OK)
    {
      ret = esp_cam_ctlr_enable(priv->csi);
    }

  if (ret != ESP_OK)
    {
      esp_cam_ctlr_del(priv->csi);
      priv->csi = NULL;
      free(priv->scratch);
      priv->scratch = NULL;
      return -EIO;
    }

  /* The capture framework can stop and restart the lower half whenever its
   * userspace buffer queue drains.  Keep the CSI/ISP shared-bridge owner for
   * the whole imgdata session instead of allocating it on every restart.
   */

  ret = esp_isp_new_processor(&isp_config, &priv->isp);
  if (ret != ESP_OK)
    {
      syslog(LOG_ERR, "ERROR: SC2336 ISP bypass setup failed: %d\n", ret);
      esp_cam_ctlr_disable(priv->csi);
      esp_cam_ctlr_del(priv->csi);
      priv->csi = NULL;
      free(priv->scratch);
      priv->scratch = NULL;
      return -EIO;
    }

  syslog(LOG_INFO,
         "SC2336 CSI: ISP RAW10 bypass, line sync packets disabled\n");

  return OK;
}

static int sc2336_data_uninit(FAR struct imgdata_s *data)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)data;

  if (priv->csi != NULL)
    {
      if (priv->receiving)
        {
          esp_cam_ctlr_stop(priv->csi);
        }

      if (priv->isp != NULL)
        {
          esp_isp_del_processor(priv->isp);
          priv->isp = NULL;
        }

      esp_cam_ctlr_disable(priv->csi);
      esp_cam_ctlr_del(priv->csi);
      priv->csi = NULL;
    }

  work_cancel(HPWORK, &priv->frame_work);
  free(priv->scratch);
  priv->scratch = NULL;
  priv->receiving = false;
  return OK;
}

static int sc2336_data_set_buf(FAR struct imgdata_s *data,
                               uint8_t nr_fmt,
                               FAR imgdata_format_t *fmt,
                               FAR uint8_t *addr, uint32_t size)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)data;

  if (addr == NULL || size < SC2336_FRAME_SIZE ||
      ((uintptr_t)addr & (SC2336_BUFFER_ALIGN - 1)) != 0)
    {
      return -EINVAL;
    }

  atomic_store_explicit(&priv->next_buffer, (uintptr_t)addr,
                        memory_order_release);
  return OK;
}

static int sc2336_validate_data(FAR struct imgdata_s *data,
                                uint8_t nr_fmt,
                                FAR imgdata_format_t *fmt,
                                FAR imgdata_interval_t *interval)
{
  if (nr_fmt != 1 || fmt[0].width != BOARD_VELAPOKA_CAMERA_WIDTH ||
      fmt[0].height != BOARD_VELAPOKA_CAMERA_HEIGHT ||
      fmt[0].pixelformat != IMGDATA_PIX_FMT_SBGGR10P ||
      interval->numerator != 1 ||
      interval->denominator != BOARD_VELAPOKA_CAMERA_FPS)
    {
      return -ENOTSUP;
    }

  return OK;
}

static int sc2336_data_start(FAR struct imgdata_s *data,
                              uint8_t nr_fmt,
                              FAR imgdata_format_t *fmt,
                              FAR imgdata_interval_t *interval,
                              FAR imgdata_capture_t callback,
                              FAR void *arg)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)data;
  int ret;

  priv->capture = callback;
  priv->capture_arg = arg;
  atomic_store_explicit(&priv->buffer_requests, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&priv->frames_finished, 0,
                        memory_order_relaxed);
  ret = esp_cam_ctlr_start(priv->csi);
  if (ret != ESP_OK)
    {
      priv->capture = NULL;
      return -EIO;
    }

  priv->receiving = true;
  return OK;
}

static int sc2336_data_stop(FAR struct imgdata_s *data)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)data;
  unsigned int requests;
  unsigned int frames;
  int ret = OK;

  frames = atomic_load_explicit(&priv->frames_finished,
                                memory_order_relaxed);
  if (frames == 0)
    {
      sc2336_dump_csi_status();
    }

  if (priv->receiving && esp_cam_ctlr_stop(priv->csi) != ESP_OK)
    {
      ret = -EIO;
    }

  requests = atomic_load_explicit(&priv->buffer_requests,
                                  memory_order_relaxed);
  frames = atomic_load_explicit(&priv->frames_finished,
                                memory_order_relaxed);
  syslog(LOG_INFO,
         "SC2336 CSI: stopped, buffer requests=%u finished frames=%u\n",
         requests, frames);

  priv->receiving = false;
  priv->capture = NULL;
  atomic_store(&priv->next_buffer, 0);
  return ret;
}

static FAR void *sc2336_data_alloc(FAR struct imgdata_s *data,
                                   uint32_t align, uint32_t size)
{
  if (align < SC2336_BUFFER_ALIGN)
    {
      align = SC2336_BUFFER_ALIGN;
    }

  return memalign(align, size);
}

static void sc2336_data_free(FAR struct imgdata_s *data, FAR void *addr)
{
  free(addr);
}

static const struct imgdata_ops_s g_sc2336_data_ops =
{
  .init                   = sc2336_data_init,
  .uninit                 = sc2336_data_uninit,
  .set_buf                = sc2336_data_set_buf,
  .validate_frame_setting = sc2336_validate_data,
  .start_capture          = sc2336_data_start,
  .stop_capture           = sc2336_data_stop,
  .alloc                  = sc2336_data_alloc,
  .free                   = sc2336_data_free,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velapoka_camera_initialize(void)
{
  const esp_ldo_channel_config_t ldo_cfg =
  {
    .chan_id = BOARD_VELAPOKA_CSI_LDO_CHANNEL,
    .voltage_mv = BOARD_VELAPOKA_CSI_LDO_MV,
  };
  FAR struct i2c_master_s *i2c;
  uint16_t pid;
  uint8_t idh;
  uint8_t idl;
  int ret;

  i2c = velapoka_i2c_initialize();
  if (i2c == NULL)
    {
      return -ENODEV;
    }

  ret = sc2336_readreg(i2c, SC2336_REG_CHIP_ID_H, &idh);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: SC2336 PID high read failed: %d\n", ret);
      return ret;
    }

  ret = sc2336_readreg(i2c, SC2336_REG_CHIP_ID_L, &idl);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: SC2336 PID low read failed: %d\n", ret);
      return ret;
    }

  pid = ((uint16_t)idh << 8) | idl;
  if (pid != BOARD_VELAPOKA_CAMERA_PID)
    {
      syslog(LOG_ERR,
             "ERROR: camera at 0x%02x has PID 0x%04x, expected 0x%04x\n",
             BOARD_VELAPOKA_CAMERA_ADDR, pid,
             BOARD_VELAPOKA_CAMERA_PID);
      return -ENODEV;
    }

  /* ESP32-P4 CSI D-PHY requires a stable 2.5 V supply.  Keep an explicit
   * camera-side reference even when the board's DSI display uses the same
   * LDO channel.  The regulator driver shares non-adjustable consumers by
   * reference count, matching esp-video's CSI device initialization.
   */

  ret = esp_ldo_acquire_channel(&ldo_cfg, &g_csi_ldo);
  if (ret != ESP_OK)
    {
      syslog(LOG_ERR, "ERROR: SC2336 CSI LDO%d %dmV acquisition failed: %d\n",
             BOARD_VELAPOKA_CSI_LDO_CHANNEL,
             BOARD_VELAPOKA_CSI_LDO_MV, ret);
      return -EIO;
    }

  nxsig_usleep(1000);
  syslog(LOG_INFO, "SC2336 CSI: LDO%d=%dmV enabled\n",
         BOARD_VELAPOKA_CSI_LDO_CHANNEL, BOARD_VELAPOKA_CSI_LDO_MV);

  ret = sc2336_write_mode(i2c);
  if (ret < 0)
    {
      return ret;
    }

  /* Keep the sensor in standby.  The CSI/V4L2 lower half starts the sensor
   * only after a capture buffer and the receiver are both ready.
   */

  ret = sc2336_writereg(i2c, SC2336_REG_STREAM, 0x00);
  if (ret < 0)
    {
      return ret;
    }

  g_sc2336_i2c = i2c;

  g_sc2336.sensor.ops              = &g_sc2336_sensor_ops;
  g_sc2336.sensor.fmtdescs_num     = 1;
  g_sc2336.sensor.fmtdescs         = g_sc2336_fmtdesc;
  g_sc2336.sensor.frmsizes_num     = 1;
  g_sc2336.sensor.frmsizes         = g_sc2336_frmsize;
  g_sc2336.sensor.frmintervals_num = 1;
  g_sc2336.sensor.frmintervals     = g_sc2336_frmival;
  g_sc2336.data.ops                = &g_sc2336_data_ops;

  {
    FAR struct imgsensor_s *sensors[1] = {&g_sc2336.sensor};

    ret = capture_register("/dev/video0", &g_sc2336.data, sensors, 1);
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: SC2336 /dev/video0 register failed: %d\n",
               ret);
        return ret;
      }
  }

  syslog(LOG_INFO,
         "SC2336: PID=0x%04x addr=0x%02x mode=%ux%u RAW%u %ufps "
         "%u-lane initialized (standby), /dev/video0 registered\n",
         pid, BOARD_VELAPOKA_CAMERA_ADDR,
         BOARD_VELAPOKA_CAMERA_WIDTH, BOARD_VELAPOKA_CAMERA_HEIGHT,
         BOARD_VELAPOKA_CAMERA_RAW_BITS, BOARD_VELAPOKA_CAMERA_FPS,
         BOARD_VELAPOKA_CAMERA_CSI_LANES);
  return OK;
}

int velapoka_camera_set_stream(bool enable)
{
  uint8_t stream;
  int ret;

  if (g_sc2336_i2c == NULL)
    {
      return -ENODEV;
    }

  ret = sc2336_writereg(g_sc2336_i2c, SC2336_REG_STREAM,
                        enable ? 0x01 : 0x00);
  if (ret < 0)
    {
      return ret;
    }

  ret = sc2336_readreg(g_sc2336_i2c, SC2336_REG_STREAM, &stream);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: SC2336 stream readback failed: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "SC2336: stream %s, reg 0x0100=0x%02x\n",
         enable ? "on" : "off", stream);
  if (stream != (enable ? 0x01 : 0x00))
    {
      return -EIO;
    }

  return OK;
}
