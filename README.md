# VelaPoka 可配置视觉装配防错终端 BSP

> 当前比赛功能方案、实板基线、里程碑进度和新会话接手入口统一维护在
> [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md)。旧章节如与计划文档冲突，
> 以计划文档中的最新状态为准。

本仓为 ESP32-P4X-Function-EV-Board 提供 openvela 板级适配和 VelaPoka
产品应用，交付两套配置：

- `configs/nsh`：已在 ESP32-P4 rev3.2 实板验证的最小 UART/NSH 恢复系统。
- `configs/velapoka`：产品配置，包含 PSRAM、GT911、MIPI-DSI framebuffer、
  SC2336/CSI、EMAC、ESP32-C6 ESP-Hosted Wi-Fi、LVGL 和 VelaPoka 应用。

当前已归档 M4 留档链路：在 M3 P0 检测基础上增加 MicroSD FAT 自动挂载、
模板和阈值恢复、JSONL、FAIL 灰度 BMP、后台存储队列及最近历史显示。
硬件 SPI2 替换 GPIO 模拟 SPI 后，最小文件和应用数据均已通过卸载、断电及
Windows 读取验收；开机自启动、独立 Exit、队列排空、FAT 卸载和返回 NSH 均已
实板通过。M5 RJ45 只读 HTTP 状态、JSONL 和 FAIL BMP 导出也已通过实板验收并
归档，覆盖自动静态 IP、访问限制、网络负载下本地检测、断线恢复和触屏安全退出；
比赛交付范围已冻结为现有固定工位 P0 检测闭环，不再把三类批量标定、四步工序、
触屏框选、定位标记校准、工程指标和长时间压力测试作为最终提交阻塞项。
产品固件在板级外设初始化完成后直接创建 VelaPoka 任务，正常使用和安全退出均可
通过触屏完成；NSH 同时保留为维护入口。
完整里程碑、已知限制和实板证据见 [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md)。

## 当前基础外设

| 外设 | 当前实现 | 验收方式 |
| --- | --- | --- |
| UART0 | 115200 8N1，GPIO37/GPIO38，NSH 恢复入口 | 上电出现 `nsh>` |
| 控制 GPIO | LCD reset GPIO27、背光 GPIO26、PHY reset GPIO51 | 初始化无错误，状态位 `0x100` |
| I2C | GPIO7 SDA、GPIO8 SCL，NuttX 软件 I2C | 状态位 `0x004` |
| GT911 | GPIO4/5 完成复位选址，地址 `0x5d`，1024x600 单指事件 | `tc 8` 四角与 DOWN/MOVE/UP 通过 |
| PSRAM | 32 MiB 外部 RAM | 相机和 framebuffer 可正常分配 |
| MIPI-DSI | EK79007，1024×600 RGB565 双 framebuffer | `/dev/fb0`；VSync 切屏实板确认无撕裂 |
| SC2336/CSI | 1280×720 BGGR packed RAW10，30 fps | `/dev/video0`；`camtest preview 3 5000` 两次通过 |
| Ethernet | NuttX `eth0`；产品自动配置 `10.0.0.2/24` 并监听 HTTP 8080 | Ping、状态、完整 JSONL、FAIL BMP、访问限制和断线恢复实板通过 |
| Wi-Fi | ESP32-C6，经 SDIO/ESP-Hosted 注册 NuttX `wlan0` | 驱动层实板通过：4-bit/20 MHz、RPC、MAC、netdev 和 ready 位；产品暂不增加联网应用 |
| MicroSD | SPI2，GPIO42/43/44/39，LDO4，FAT32 | `/dev/mmcsd0`、自动挂载、断电后 Windows 读取通过 |
| LVGL | 1024×600 左右分屏产品界面 | 产品固件开机自启动；触摸正常，灰度预览稳定 15 fps 且无撕裂 |
| BSP 状态 | 只读 JSON 字符设备 | `cat /dev/velapoka` |

官方开发板说明和参考 BSP分别见 [ESP32-P4X-Function-EV-Board 用户指南](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-function-ev-board/user_guide.html) 与 [Espressif esp-bsp](https://github.com/espressif/esp-bsp)。

## 目录结构

- `board/contest_board/chips/esp32p4/`：启动、中断、UART、系统 tick 和 ESP HAL 兼容层。
- `board/contest_board/common/`：板级公共初始化与链接脚本。
- `board/contest_board/src/velapoka_bsp.c`：控制 GPIO、共享 I2C 和状态节点。
- `board/contest_board/src/velapoka_gt911.c`：GT911 NuttX touchscreen lower-half。
- `board/contest_board/src/velapoka_display.c`：EK79007 MIPI-DSI framebuffer。
- `board/contest_board/src/velapoka_sc2336.c`：SC2336 与 ESP32-P4 CSI bridge。
- `board/contest_board/docs/display-touch-bringup.md`：显示/触摸 bring-up 开发文档。
- `board/contest_board/docs/m2-camera-preview-archive.md`：M2 实时预览实板验收归档。
- `board/contest_board/docs/m4-storage-archive.md`：M4 MicroSD 留档、恢复和安全退出归档。
- `board/contest_board/docs/m5-ethernet-export-archive.md`：M5 RJ45 查询、JSONL/BMP 导出、访问保护和断线恢复归档。
- `board/contest_board/docs/m5-ethernet-export-test.md`：M5 RJ45 状态、JSONL、FAIL BMP 和断网实板验收步骤。
- `board/contest_board/docs/pre-network-export-baseline-archive.md`：RJ45 数据导出开发前的离线产品基线、未完成项和后续边界。
- `board/contest_board/docs/velapoka-capture-storage-chain.md`：采集、检测与 SD 写入全链路。
- `board/contest_board/docs/wifi-esp-hosted-bringup.md`：ESP32-C6 SDIO/ESP-Hosted 驱动边界与验收步骤。
- `board/contest_board/include/board.h`：VelaPoka 板级资源表。
- `board/contest_board/configs/{nsh,velapoka}/defconfig`：恢复配置与产品基础配置。
- `board/contest_board/upstream/nuttx/`：构建基线所需的公共 NuttX 修复。
- `app/camtest/`：可交付的 V4L2/RAW10 相机 smoke test。
- `app/velapoka/`：LVGL 产品应用、V4L2 相机线程、P0 检测、MicroSD 留档和 RJ45 HTTP 导出。

## 构建

在 openvela 工作区根目录执行：

```bash
# 公共修复尚未合入基线时，仅首次执行。
git -C nuttx apply \
  ../vendor/openvela/boards/contest2026_284_board/upstream/nuttx/0001-riscv-espressif-fix-kconfig-menu.patch \
  ../vendor/openvela/boards/contest2026_284_board/upstream/nuttx/0002-usrsock-guard-api-when-disabled.patch \
  ../vendor/openvela/boards/contest2026_284_board/upstream/nuttx/0003-mmcsd-spi-verify-write-completion.patch

# 镜像打包需要 esptool >= 4.8。
python3 -m pip install 'esptool>=4.8,<5'

./build.sh vendor/openvela/boards/contest2026_284_board/configs/nsh -j2
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
ls /dev/video0
camtest preview 3 5000
velapoka
```

`/dev/velapoka` 的 `capabilities` 表示板上规划资源，`ready` 只表示本次启动
已成功初始化的功能，应以每次启动的实际值为准。单个非关键外设失败时仍保留
NSH 恢复入口。

## 当前开发重点

M4 和 M5 核心链路均已归档，比赛功能范围已经冻结。剩余工作仅为整理 5 分钟内
演示流程和视频、复核构建与操作文档、归集 AI Coding 日志并推送最终版本。
图形化导出网页属于可选增强，现有 JSON/JSONL/BMP API 已满足核心查询与导出功能。
