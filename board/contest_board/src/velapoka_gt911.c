/****************************************************************************
 * vendor/openvela/boards/contest2026_284_board/src/velapoka_gt911.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/arch.h>
#include <nuttx/wqueue.h>

#include <arch/board/board.h>

#include <arch/board/velapoka_bsp.h>

#include "espressif/esp_gpio.h"

#define GT911_STATUS_REG       0x814e
#define GT911_POINT_REG        0x814f
#define GT911_PRODUCT_ID_REG   0x8140
#define GT911_CONFIG_REG       0x8047
#define GT911_MAX_POINTS       5
#define GT911_POINT_BYTES      8
#define GT911_BUFFER_SIZE      (1 + GT911_MAX_POINTS * GT911_POINT_BYTES)
#define GT911_POWERON_DELAY_MS 120
#define GT911_PROBE_RETRIES    3
#define GT911_PROBE_DELAY_MS   20

static void gt911_select_address(uint8_t addr)
{
  bool int_level = addr == BOARD_VELAPOKA_TOUCH_ALT_ADDR;

  esp_configgpio(BOARD_VELAPOKA_TOUCH_RESET, OUTPUT);
  esp_configgpio(BOARD_VELAPOKA_TOUCH_INT, OUTPUT);
  esp_gpiowrite(BOARD_VELAPOKA_TOUCH_RESET, false);
  esp_gpiowrite(BOARD_VELAPOKA_TOUCH_INT, false);
  up_mdelay(10);

  esp_gpiowrite(BOARD_VELAPOKA_TOUCH_INT, int_level);
  up_mdelay(1);
  esp_gpiowrite(BOARD_VELAPOKA_TOUCH_RESET, true);
  up_mdelay(10);
  up_mdelay(50);
  esp_configgpio(BOARD_VELAPOKA_TOUCH_INT, INPUT);
}

struct gt911_dev_s
{
  struct touch_lowerhalf_s lower;
  struct i2c_master_s *i2c;
  struct work_s work;
  uint8_t addr;
  bool contact;
  int16_t last_x;
  int16_t last_y;
  uint8_t last_id;
  uint8_t buffer[GT911_BUFFER_SIZE];
};

static struct gt911_dev_s g_gt911;

static int gt911_read(FAR struct gt911_dev_s *dev, uint16_t reg,
                      FAR uint8_t *buffer, size_t buflen)
{
  uint8_t regbuf[2] = {reg >> 8, reg & 0xff};
  struct i2c_msg_s msgs[2] =
  {
    {
      .frequency = BOARD_VELAPOKA_TOUCH_FREQUENCY,
      .addr = dev->addr,
      .flags = I2C_M_NOSTOP,
      .buffer = regbuf,
      .length = sizeof(regbuf),
    },
    {
      .frequency = BOARD_VELAPOKA_TOUCH_FREQUENCY,
      .addr = dev->addr,
      .flags = I2C_M_READ,
      .buffer = buffer,
      .length = buflen,
    }
  };

  return I2C_TRANSFER(dev->i2c, msgs, 2);
}

static int gt911_write_u8(FAR struct gt911_dev_s *dev, uint16_t reg,
                          uint8_t value)
{
  uint8_t buffer[3] = {reg >> 8, reg & 0xff, value};
  struct i2c_msg_s msg =
  {
    .frequency = BOARD_VELAPOKA_TOUCH_FREQUENCY,
    .addr = dev->addr,
    .flags = 0,
    .buffer = buffer,
    .length = sizeof(buffer),
  };

  return I2C_TRANSFER(dev->i2c, &msg, 1);
}

static void gt911_scan_bus(FAR struct gt911_dev_s *dev)
{
  struct i2c_msg_s msg =
  {
    .frequency = BOARD_VELAPOKA_TOUCH_FREQUENCY,
    .flags = 0,
    .buffer = NULL,
    .length = 0,
  };
  bool found = false;
  uint8_t addr;

  syslog(LOG_INFO, "GT911: scanning I2C bus");
  for (addr = 0x08; addr <= 0x77; addr++)
    {
      msg.addr = addr;
      if (I2C_TRANSFER(dev->i2c, &msg, 1) >= 0)
        {
          syslog(LOG_INFO, " 0x%02x", addr);
          found = true;
        }
    }

  syslog(LOG_INFO, found ? "\n" : " no devices found\n");
}

static uint16_t gt911_get_le16(FAR const uint8_t *value)
{
  return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static void gt911_report(FAR struct gt911_dev_s *dev, bool down,
                         FAR const uint8_t *point_data)
{
  struct touch_sample_s sample;
  FAR struct touch_point_s *point = &sample.point[0];
  uint16_t raw_x;
  uint16_t raw_y;
  int16_t x;
  int16_t y;

  memset(&sample, 0, sizeof(sample));
  sample.npoints = 1;

  if (down)
    {
      raw_x = gt911_get_le16(point_data + 1);
      raw_y = gt911_get_le16(point_data + 3);
      dev->last_id = point_data[0];

      /* The touch FPC is rotated 180 degrees relative to the displayed
       * framebuffer.  Convert GT911 raw coordinates to LVGL screen space.
       */

      if (raw_x >= BOARD_VELAPOKA_LCD_WIDTH)
        {
          raw_x = BOARD_VELAPOKA_LCD_WIDTH - 1;
        }

      if (raw_y >= BOARD_VELAPOKA_LCD_HEIGHT)
        {
          raw_y = BOARD_VELAPOKA_LCD_HEIGHT - 1;
        }

      x = BOARD_VELAPOKA_LCD_WIDTH - 1 - raw_x;
      y = BOARD_VELAPOKA_LCD_HEIGHT - 1 - raw_y;

      if (dev->contact && dev->last_id == point_data[0] &&
          dev->last_x == x && dev->last_y == y)
        {
          return;
        }

      dev->last_x = x;
      dev->last_y = y;

      if (!dev->contact)
        {
          syslog(LOG_INFO, "GT911: DOWN id=%u x=%d y=%d\n",
                 dev->last_id, x, y);
        }
    }
  else
    {
      syslog(LOG_INFO, "GT911: UP id=%u x=%d y=%d\n",
             dev->last_id, dev->last_x, dev->last_y);
    }

  point->id = dev->last_id;
  point->x = dev->last_x;
  point->y = dev->last_y;
  point->pressure = down ? 1 : 0;
  point->flags = TOUCH_ID_VALID | TOUCH_POS_VALID |
                 TOUCH_PRESSURE_VALID;
  point->flags |= down ? (dev->contact ? TOUCH_MOVE : TOUCH_DOWN) : TOUCH_UP;
  dev->contact = down;
  touch_event(dev->lower.priv, &sample);
}

static void gt911_worker(FAR void *arg)
{
  FAR struct gt911_dev_s *dev = arg;
  uint8_t points;
  bool valid;
  int ret;

  ret = gt911_read(dev, GT911_STATUS_REG, dev->buffer, 1);
  if (ret < 0)
    {
      ierr("GT911 status read failed: %d\n", ret);
      goto queue_again;
    }

  valid = (dev->buffer[0] & 0x80) != 0;
  points = dev->buffer[0] & 0x0f;
  if (!valid)
    {
      goto queue_again;
    }

  if (points > 0 && points <= GT911_MAX_POINTS)
    {
      ret = gt911_read(dev, GT911_POINT_REG, &dev->buffer[1],
                       points * GT911_POINT_BYTES);
      if (ret >= 0)
        {
          gt911_report(dev, true, &dev->buffer[1]);
        }
    }
  else if (points == 0 && dev->contact)
    {
      gt911_report(dev, false, NULL);
    }

  if (valid)
    {
      ret = gt911_write_u8(dev, GT911_STATUS_REG, 0);
      if (ret < 0)
        {
          ierr("GT911 status clear failed: %d\n", ret);
        }
    }

queue_again:
  ret = work_queue(LPWORK, &dev->work, gt911_worker, dev,
                   CONFIG_VELAPOKA_TOUCHSCREEN_SAMPLE_DELAY);
  if (ret < 0)
    {
      ierr("GT911 work queue failed: %d\n", ret);
    }
}

int velapoka_touchscreen_initialize(void)
{
  FAR struct gt911_dev_s *dev = &g_gt911;
  static const uint8_t addresses[] =
  {
    BOARD_VELAPOKA_TOUCH_ADDR,
    BOARD_VELAPOKA_TOUCH_ALT_ADDR
  };
  uint8_t product_id[4];
  uint8_t config[5];
  size_t i;
  int retry;
  int ret;

  memset(dev, 0, sizeof(*dev));
  dev->i2c = velapoka_i2c_initialize();
  if (dev->i2c == NULL)
    {
      return -ENODEV;
    }

  /* INT and RESET are wired from the touch FPC to expansion-header GPIOs.
   * Drive the GT911 address-selection sequence before probing each address.
   */

  up_mdelay(GT911_POWERON_DELAY_MS);
  syslog(LOG_INFO, "GT911: I2C GPIO SCL%d=%d SDA%d=%d\n",
         BOARD_VELAPOKA_I2C_SCL,
         esp_gpioread(BOARD_VELAPOKA_I2C_SCL),
         BOARD_VELAPOKA_I2C_SDA,
         esp_gpioread(BOARD_VELAPOKA_I2C_SDA));
  ret = -ENODEV;

  for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++)
    {
      dev->addr = addresses[i];
      gt911_select_address(dev->addr);

      for (retry = 0; retry < GT911_PROBE_RETRIES; retry++)
        {
          ret = gt911_read(dev, GT911_PRODUCT_ID_REG, product_id,
                           sizeof(product_id));
          if (ret >= 0)
            {
              goto found;
            }

          up_mdelay(GT911_PROBE_DELAY_MS);
        }
    }

  syslog(LOG_ERR, "ERROR: GT911 not found at I2C addresses 0x%02x/0x%02x\n",
         BOARD_VELAPOKA_TOUCH_ADDR, BOARD_VELAPOKA_TOUCH_ALT_ADDR);
  gt911_scan_bus(dev);
  return ret;

found:

  ret = gt911_read(dev, GT911_CONFIG_REG, config, sizeof(config));
  if (ret >= 0)
    {
      syslog(LOG_INFO, "GT911: config=%u raw resolution=%ux%u\n",
             config[0], gt911_get_le16(&config[1]),
             gt911_get_le16(&config[3]));
    }

  ret = gt911_write_u8(dev, GT911_STATUS_REG, 0);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: GT911 initial status clear failed: %d\n",
             ret);
    }

  dev->lower.maxpoint = 1;
  ret = touch_register(&dev->lower, CONFIG_VELAPOKA_TOUCHSCREEN_PATH,
                       CONFIG_VELAPOKA_TOUCHSCREEN_SAMPLE_CACHES);
  if (ret < 0)
    {
      return ret;
    }

  ret = work_queue(LPWORK, &dev->work, gt911_worker, dev,
                   CONFIG_VELAPOKA_TOUCHSCREEN_SAMPLE_DELAY);
  if (ret < 0)
    {
      return ret;
    }

  velapoka_bsp_mark_ready(VELAPOKA_CAP_TOUCH);
  syslog(LOG_INFO, "GT911: product %.4s at 0x%02x registered at %s\n",
         product_id, dev->addr, CONFIG_VELAPOKA_TOUCHSCREEN_PATH);
  return OK;
}
