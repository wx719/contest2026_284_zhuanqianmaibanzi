/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/include/board.h
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

#ifndef __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_INCLUDE_BOARD_H
#define __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_INCLUDE_BOARD_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GPIO pins used by the GPIO Subsystem */

#define BOARD_NGPIOOUT    2 /* Amount of GPIO Output pins */
#define BOARD_NGPIOINT    1 /* Amount of GPIO Input w/ Interruption pins */

/* ESP32P4-Generic GPIOs ****************************************************/

/* BOOT Button */

#define BUTTON_BOOT  35

/* VelaPoka fixed-workstation hardware map.  Values are taken from the
 * ESP32-P4X-Function-EV-Board reference BSP and schematic.  MIPI lane pins
 * are dedicated pads and are intentionally not represented as GPIOs.
 */

#define BOARD_VELAPOKA_I2C_PORT          1
#define BOARD_VELAPOKA_I2C_SDA           7
#define BOARD_VELAPOKA_I2C_SCL           8

#define BOARD_VELAPOKA_LCD_WIDTH         1024
#define BOARD_VELAPOKA_LCD_HEIGHT        600
#define BOARD_VELAPOKA_LCD_RESET         27
#define BOARD_VELAPOKA_LCD_BACKLIGHT     26
#define BOARD_VELAPOKA_LCD_DSI_LANES     2
#define BOARD_VELAPOKA_LCD_DSI_MBPS      1000
#define BOARD_VELAPOKA_DSI_LDO_CHANNEL   3
#define BOARD_VELAPOKA_DSI_LDO_MV        2500

#define BOARD_VELAPOKA_TOUCH_ADDR        0x5d
#define BOARD_VELAPOKA_TOUCH_ALT_ADDR    0x14
#define BOARD_VELAPOKA_TOUCH_FREQUENCY   100000
#define BOARD_VELAPOKA_TOUCH_INT         4
#define BOARD_VELAPOKA_TOUCH_RESET       5

#define BOARD_VELAPOKA_CAMERA_ADDR       0x30
#define BOARD_VELAPOKA_CAMERA_FREQUENCY  100000
#define BOARD_VELAPOKA_CAMERA_PID        0xcb3a
#define BOARD_VELAPOKA_CAMERA_WIDTH      1280
#define BOARD_VELAPOKA_CAMERA_HEIGHT     720
#define BOARD_VELAPOKA_CAMERA_FPS        30
#define BOARD_VELAPOKA_CAMERA_RAW_BITS   10
#define BOARD_VELAPOKA_CAMERA_CSI_LANES  2
#define BOARD_VELAPOKA_CSI_LDO_CHANNEL   3
#define BOARD_VELAPOKA_CSI_LDO_MV        2500

#define BOARD_VELAPOKA_SD_D0             39
#define BOARD_VELAPOKA_SD_D1             40
#define BOARD_VELAPOKA_SD_D2             41
#define BOARD_VELAPOKA_SD_D3             42
#define BOARD_VELAPOKA_SD_CLK            43
#define BOARD_VELAPOKA_SD_CMD            44
#define BOARD_VELAPOKA_SD_LDO_CHANNEL    4
#define BOARD_VELAPOKA_SD_LDO_MV         3300

#define BOARD_VELAPOKA_WIFI_SDIO_CLK     18
#define BOARD_VELAPOKA_WIFI_SDIO_CMD     19
#define BOARD_VELAPOKA_WIFI_SDIO_D0      14
#define BOARD_VELAPOKA_WIFI_SDIO_D1      15
#define BOARD_VELAPOKA_WIFI_SDIO_D2      16
#define BOARD_VELAPOKA_WIFI_SDIO_D3      17
#define BOARD_VELAPOKA_WIFI_RESET        54

#define BOARD_VELAPOKA_ETH_MDC           31
#define BOARD_VELAPOKA_ETH_MDIO          52
#define BOARD_VELAPOKA_ETH_REF_CLK       50
#define BOARD_VELAPOKA_ETH_TX_EN         49
#define BOARD_VELAPOKA_ETH_TXD0          34
#define BOARD_VELAPOKA_ETH_TXD1          35
#define BOARD_VELAPOKA_ETH_CRS_DV        28
#define BOARD_VELAPOKA_ETH_RXD0          29
#define BOARD_VELAPOKA_ETH_RXD1          30
#define BOARD_VELAPOKA_ETH_PHY_RESET     51
#define BOARD_VELAPOKA_ETH_PHY_ADDR      1

#endif /* __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_INCLUDE_BOARD_H */
