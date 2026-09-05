/****************************************************************************
 * vendor/openvela/boards/contest2026_284_board/include/velapoka_bsp.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_OPENVELA_BOARDS_CONTEST2026_284_INCLUDE_VELAPOKA_BSP_H
#define __VENDOR_OPENVELA_BOARDS_CONTEST2026_284_INCLUDE_VELAPOKA_BSP_H

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

struct i2c_master_s;

/* Hardware physically present on the selected Function EV Board assembly. */

#define VELAPOKA_CAP_UART          (1u << 0)
#define VELAPOKA_CAP_PSRAM         (1u << 1)
#define VELAPOKA_CAP_I2C           (1u << 2)
#define VELAPOKA_CAP_TOUCH         (1u << 3)
#define VELAPOKA_CAP_MIPI_DSI      (1u << 4)
#define VELAPOKA_CAP_MIPI_CSI      (1u << 5)
#define VELAPOKA_CAP_MICROSD       (1u << 6)
#define VELAPOKA_CAP_ETHERNET      (1u << 7)
#define VELAPOKA_CAP_CONTROL_GPIO  (1u << 8)
#define VELAPOKA_CAP_WIFI          (1u << 9)

#define VELAPOKA_CAPABILITIES      (VELAPOKA_CAP_UART | \
                                    VELAPOKA_CAP_PSRAM | \
                                    VELAPOKA_CAP_I2C | \
                                    VELAPOKA_CAP_TOUCH | \
                                    VELAPOKA_CAP_MIPI_DSI | \
                                    VELAPOKA_CAP_MIPI_CSI | \
                                    VELAPOKA_CAP_MICROSD | \
                                    VELAPOKA_CAP_ETHERNET | \
                                    VELAPOKA_CAP_CONTROL_GPIO | \
                                    VELAPOKA_CAP_WIFI)

/* Bits in the ready mask use the same values as the capability mask. */

uint32_t velapoka_bsp_capabilities(void);
uint32_t velapoka_bsp_ready_mask(void);
void velapoka_bsp_mark_ready(uint32_t mask);
struct i2c_master_s *velapoka_i2c_initialize(void);

int velapoka_lcd_reset(void);
int velapoka_lcd_backlight(bool enable);
int velapoka_display_initialize(void);
int velapoka_bsp_initialize(void);

#ifdef CONFIG_VELAPOKA_TOUCHSCREEN
int velapoka_touchscreen_initialize(void);
#endif

#ifdef CONFIG_VELAPOKA_CAMERA
int velapoka_camera_initialize(void);
int velapoka_camera_set_stream(bool enable);
#endif

#endif
