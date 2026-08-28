# VelaPoka 可配置视觉装配防错终端 BSP

> 当前比赛功能方案、实板基线、里程碑进度和新会话接手入口统一维护在
> [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md)。旧章节如与计划文档冲突，
> 以计划文档中的最新状态为准。

本仓为 ESP32-P4X-Function-EV-Board 提供 openvela 板级适配。当前阶段按“先打通基础外设”的原则交付两套配置：

- `configs/nsh`：已在 ESP32-P4 rev3.2 实板验证的最小 UART/NSH 恢复系统。
- `configs/velapoka`：VelaPoka 实板配置，包含控制 GPIO、GPIO7/8 软件 I2C、GT911 触摸和 EK79007 MIPI-DSI 彩条 first-light。

MIPI-DSI 显示与 GT911 触摸已经完成实板验收；MIPI-CSI 摄像头、SDMMC、以太网和 PSRAM 将继续按设备节点和实板 smoke test 逐项接入。

## 当前基础外设

| 外设 | 当前实现 | 验收方式 |
| --- | --- | --- |
| UART0 | 115200 8N1，GPIO37/GPIO38，NSH 恢复入口 | 上电出现 `nsh>` |
| 控制 GPIO | LCD reset GPIO27、背光 GPIO26、PHY reset GPIO51 | 初始化无错误，状态位 `0x100` |
| I2C | GPIO7 SDA、GPIO8 SCL，NuttX 软件 I2C | 状态位 `0x004` |
| GT911 | GPIO4/5 完成复位选址，地址 `0x5d`，1024x600 单指事件 | `tc 8` 四角与 DOWN/MOVE/UP 通过 |
| MIPI-DSI | EK79007，2 Lane 1000 Mbps，RGB888，硬件彩条 | 屏幕稳定显示彩条，状态位 `0x010` |
| BSP 状态 | 只读 JSON 字符设备 | `cat /dev/velapoka` |

官方开发板说明和参考 BSP分别见 [ESP32-P4X-Function-EV-Board 用户指南](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-function-ev-board/user_guide.html) 与 [Espressif esp-bsp](https://github.com/espressif/esp-bsp)。

## 目录结构

- `board/contest_board/chips/esp32p4/`：启动、中断、UART、系统 tick 和 ESP HAL 兼容层。
- `board/contest_board/common/`：板级公共初始化与链接脚本。
- `board/contest_board/src/velapoka_bsp.c`：控制 GPIO、共享 I2C 和状态节点。
- `board/contest_board/src/velapoka_gt911.c`：GT911 NuttX touchscreen lower-half。
- `board/contest_board/src/velapoka_display.c`：EK79007 MIPI-DSI first-light。
- `board/contest_board/docs/display-touch-bringup.md`：显示/触摸 bring-up 开发文档。
- `board/contest_board/include/board.h`：VelaPoka 板级资源表。
- `board/contest_board/configs/{nsh,velapoka}/defconfig`：恢复配置与产品基础配置。
- `board/contest_board/upstream/nuttx/`：构建基线所需的公共 NuttX 修复。

## 构建

在 openvela 工作区根目录执行：

```bash
# 公共修复尚未合入基线时，仅首次执行。
git -C nuttx apply \
  ../vendor/openvela/boards/contest2026_284_board/upstream/nuttx/0001-riscv-espressif-fix-kconfig-menu.patch \
  ../vendor/openvela/boards/contest2026_284_board/upstream/nuttx/0002-usrsock-guard-api-when-disabled.patch

# 镜像打包需要 esptool >= 4.8。
python3 -m pip install 'esptool>=4.8,<5'

./build.sh vendor/openvela/boards/contest2026_284_board/configs/velapoka -j2
```

生成 `nuttx/vela_nuttx.bin`。如需回到最小恢复系统，将最后一条命令中的 `velapoka` 改为 `nsh`。

## 烧录与基础验收

simple boot 镜像写入 `0x2000`：

```bash
esptool.py -c esp32p4 -p /dev/ttyACM0 -b 921600 write_flash \
  -fs 4MB -fm dio -ff 80m 0x2000 nuttx/vela_nuttx.bin

picocom -b 115200 /dev/ttyUSB0
```

进入 NSH 后执行：

```text
help
uname -a
uptime
cat /dev/velapoka
ls /dev/input0
```

`/dev/velapoka` 的 `capabilities` 表示板上规划资源，`ready` 只表示本次启动已成功初始化的功能。显示与触摸全部通过时 `ready` 为 `0x0000011d`。触摸失败是非致命错误，显示 first-light 仍会继续；显示初始化失败则保留错误并返回 NSH。

## 后续接入顺序

显示与触摸实板验证过程见 [bring-up 文档](board/contest_board/docs/display-touch-bringup.md)。后续按摄像头 → MicroSD → PSRAM → 以太网的顺序增加驱动；每一项只有在形成 NuttX 设备节点并完成实板 smoke test 后，才会加入 `ready` 位。
