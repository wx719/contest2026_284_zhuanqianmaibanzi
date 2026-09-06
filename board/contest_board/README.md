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
- `src/velapoka_display.c`：EK79007 MIPI-DSI framebuffer 与 VSync 切换。
- `src/velapoka_sc2336.c`：SC2336 传感器、CSI bridge 和 V4L2 注册。
- `src/velapoka_hosted_sdio.c`：ESP32-P4 SDMMC host 与板载 ESP32-C6 SDIO 链路。
- `src/velapoka_esp_hosted.c`：ESP-Hosted RPC、数据通道和 NuttX `wlan0` netdev。
- `docs/display-touch-bringup.md`：显示与触摸链路、调试和实板验收流程。
- `docs/wifi-esp-hosted-bringup.md`：Wi-Fi 驱动架构、配置和实板验收流程。
- `scripts/Make.defs`：链接规则、simple boot 镜像生成和 `vela_nuttx.bin` 产物命名。
- `upstream/nuttx/`：需要单独提交到公共 NuttX 仓的基线修复。

构建、烧录和验证步骤见仓库根目录 [README](../../README.md)。

当前 UART、控制 GPIO、I2C、GT911、PSRAM、MIPI-DSI framebuffer、SC2336/CSI、
MicroSD 和 EMAC 链路均已完成实板 smoke test。显示注册为 `/dev/fb0`，相机注册为
`/dev/video0`，两次 `camtest preview 3 5000` 均获得完整 1152000 字节帧；
MicroSD 通过 SPI2 注册为 `/dev/mmcsd0`，FAT32 文件在卸载、断电后可由 Windows
读取；EMAC 注册为 `eth0`，静态 IPv4 双向 Ping 和产品 HTTP 查询/导出均已通过。
板载 ESP32-C6 已通过 SDIO 和 ESP-Hosted 接入 `wlan0`；实板已完成
transport/RPC、STA 启动、MAC 获取、netdev 注册和 BSP ready 位验收。本阶段未
加入 Wi-Fi 联网应用。
