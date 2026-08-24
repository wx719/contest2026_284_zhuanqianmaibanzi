# ESP32-P4 Function EV Board

本目录由赛事 manifest 映射到：

```text
vendor/openvela/boards/contest2026_284_board
```

所有芯片和板级定制代码均保存在该 vendor 映射目录中，未把作品代码放入 NuttX 核心目录。

## 组成

- `chips/esp32p4/`：ESP32-P4 启动、中断、UART、系统定时器及 HAL 兼容层。
- `common/`：Espressif 板级公共初始化和链接脚本。
- `src/`、`include/`：Function EV Board 初始化及引脚定义。
- `configs/nsh/defconfig`：赛事 L0 最小 NSH 配置。
- `configs/velapoka/defconfig`：VelaPoka 基础外设配置。
- `src/velapoka_bsp.c`：控制 GPIO、共享软件 I2C 和状态节点。
- `src/velapoka_gt911.c`：GT911 触控探测与轮询 lower-half。
- `src/velapoka_display.c`：EK79007 MIPI-DSI first-light 与硬件彩条。
- `docs/display-touch-bringup.md`：显示与触摸链路、调试和实板验收流程。
- `scripts/Make.defs`：链接规则、simple boot 镜像生成和 `vela_nuttx.bin` 产物命名。
- `upstream/nuttx/`：需要单独提交到公共 NuttX 仓的基线修复。

构建、烧录和验证步骤见仓库根目录 [README](../../README.md)。

当前 UART、控制 GPIO、I2C、GT911、MIPI-DSI 彩条、PSRAM 和 EMAC 链路已完成实板验证。其中 EMAC 已注册为 NuttX `eth0`，静态 IPv4 下与 Windows 主机双向 Ping 均为 0% 丢包。CSI 和 SDMMC 仍未完成对应设备节点与实板 smoke test，不能把 HAL 源码存在等同于 NuttX 驱动就绪。
