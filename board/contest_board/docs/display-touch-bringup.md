# VelaPoka MIPI-DSI Display and Touch Bring-up Guide

本文记录 VelaPoka 在 ESP32-P4X-Function-EV-Board 上适配 1024x600
MIPI-DSI 屏幕的实板流程，包括 EK79007 first-light 和 GT911 单点触摸。

## 1. 实板结果

| 项目 | 已验证配置 |
| --- | --- |
| 显示控制器 | EK79007，屏上搭配 EK73217B Gate Driver |
| 分辨率 | 1024x600 |
| DSI | 2 Lane，1000 Mbps/Lane，RGB888 Video Burst |
| DPI 时钟 | 52 MHz |
| First-light | ESP32-P4 DSI 硬件彩条 |
| 触摸控制器 | GT911，Product ID `911` |
| 触摸地址 | GPIO 复位选址后的 `0x5d` |
| 触摸配置 | Version 89，1024x600 |
| 设备节点 | `/dev/input0` |
| 坐标变换 | 不交换 X/Y，不镜像 X/Y |

完整成功日志：

```text
GT911: I2C GPIO SCL8=1 SDA7=1
GT911: config=89 raw resolution=1024x600
GT911: product 911 at 0x5d registered at /dev/input0
VelaPoka display: EK79007 color bars active
VelaPoka BSP: capabilities=000001ff ready=0000011d
```

## 2. 硬件连接

### 2.1 DSI 与触摸 I2C

| 功能 | 连接 |
| --- | --- |
| DSI D0/D1/CLK | ESP32-P4 MIPI 专用 Pad |
| Touch SCL | DSI Pin 11，GPIO8 |
| Touch SDA | DSI Pin 12，GPIO7 |
| Touch 3.3 V | DSI Pin 14/15 |
| GND | DSI Pin 1/4/7/10/13 |
| LCD Reset | GPIO27 |
| Backlight Enable | GPIO26 |

### 2.2 六针触摸 FPC

DSI 座没有引出触摸 INT/RESET，需要从触摸接线座用杜邦线连接 J1：

| FPC | 信号 | 板端连接 |
| --- | --- | --- |
| Pin 1 | 3.3 V | DSI 3.3 V |
| Pin 2 | SCL | GPIO8 / DSI Pin 11 |
| Pin 3 | SDA | GPIO7 / DSI Pin 12 |
| Pin 4 | INT_TP | GPIO4 / J1 Pin 18 |
| Pin 5 | RESET_TP | GPIO5 / J1 Pin 16 |
| Pin 6 | GND | GND |

更改杜邦线前必须断电，并按 FPC 触点面确认针脚方向。

## 3. 软件调用链

```text
esp32p4_appinit
  -> board_late_initialize
    -> velapoka_bsp_initialize
      -> 控制 GPIO 与软件 I2C
      -> velapoka_touchscreen_initialize
        -> GT911 复位选址
        -> NuttX touchscreen lower-half 注册
      -> velapoka_display_initialize
        -> D-PHY LDO
        -> ESP32-P4 MIPI-DSI Host
        -> DPI Video Timing
        -> NuttX mipi_dsi_device attach
        -> EK79007 DCS 初始化
        -> DSI 硬件彩条
        -> Display On 与背光
```

| 文件 | 职责 |
| --- | --- |
| `src/velapoka_bsp.c` | 初始化顺序、GPIO、共享 I2C、ready 状态 |
| `src/velapoka_display.c` | D-PHY、DPI、EK79007、first-light |
| `src/velapoka_gt911.c` | 复位选址、轮询和触摸事件 |
| `chips/esp32p4/espressif/esp_mipi_dsi.c` | ESP32-P4 DSI Host/HAL 对接 |
| `include/board.h` | 引脚、分辨率、Lane Rate、触摸地址 |

## 4. 显示 Bring-up

### 4.1 初始化前保持黑屏

先配置 Reset 和 Backlight GPIO，背光保持关闭，直到 DSI 视频流和 Panel
均初始化完成，避免上电时出现亮白屏或不稳定画面。

### 4.2 D-PHY 供电与 Panel Reset

申请 LDO Channel 3，电压 2500 mV。GPIO27 拉低 20 ms，再拉高并等待
120 ms。LDO 或 Reset 失败必须终止显示初始化。

### 4.3 DSI/DPI 参数

```text
Resolution:       1024 x 600
Pixel Format:     RGB888
Data Lanes:       2
Lane Bit Rate:    1000 Mbps
Pixel Clock:      52 MHz
HSYNC:            Pulse 10, Back Porch 160, Front Porch 160
VSYNC:            Pulse 1,  Back Porch 23,  Front Porch 12
Virtual Channel:  0
Mode:             Video + Burst + Sync Pulse + LPM
```

初始化 ESP32-P4 DSI Host 和 DPI 后，注册 NuttX `mipi_dsi_device`，设置
Lane、Format、Mode Flags 并 attach 到 Host。

### 4.4 EK79007 与 First-light

发送以 `0xb2` 开头的厂商命令序列，发送 `MIPI_DCS_EXIT_SLEEP_MODE`
后等待 120 ms。随后启动 DSI 硬件彩条，发送 Display On，等待 20 ms，
最后开启背光。

硬件彩条用于隔离验证 Panel/DSI 链路，不依赖 Framebuffer、DMA2D 或 GUI。
只有该路径稳定后，才继续接入图形栈。

## 5. 触摸 Bring-up

### 5.1 验证 I2C

软件 I2C 使用 GPIO8/GPIO7，100 kHz，开漏。空闲时 SCL/SDA 都应为高。
扫描可能发现板载音频 Codec `0x18`，它不是触摸芯片，也不能证明屏端支路连通。

GT911 使用 16 位寄存器地址。写寄存器地址后读数据必须使用 Repeated START，
因此 NuttX 第一条 Message 需要 `I2C_M_NOSTOP`。

### 5.2 GT911 复位选址

```text
RESET=0, INT=0, 等待 10 ms
INT 切换为地址选择电平，等待 1 ms
RESET=1，等待 60 ms
INT 配置为浮空输入
```

INT 低选择 `0x5d`，INT 高选择 `0x14`。驱动可尝试两个地址，最终接线配置
稳定工作在 `0x5d`。

### 5.3 NuttX Input 事件

读取 `0x8140` Product ID 和 `0x8047` 配置，清除 `0x814e` 状态，设置
`lower.maxpoint = 1`，注册 `/dev/input0`。

Worker 每 10 ms 轮询。只有 Data Ready 时才处理数据；有效状态且点数变为 0
才产生 UP；未就绪状态不产生事件；坐标不变时不重复发送 MOVE。

| Flags | 事件 |
| --- | --- |
| `0x39` | DOWN + ID/POS/PRESSURE Valid |
| `0x3a` | MOVE + ID/POS/PRESSURE Valid |
| `0x3c` | UP + ID/POS/PRESSURE Valid |

本屏 FPC 的坐标方向与显示方向一致：

```text
swap_xy  = 0
mirror_x = 0
mirror_y = 0
```

## 6. 构建与烧录

在 openvela 工作区根目录执行：

```bash
./build.sh vendor/openvela/boards/contest2026_284_board/configs/velapoka -j8
```

已配置的 NuttX 目录可增量构建：

```bash
make -C nuttx EXTRAFLAGS='-Wno-cpp -Wno-deprecated-declarations' -j8
```

Simple Boot 镜像写入 `0x2000`：

```bash
esptool.py -c esp32p4 -p /dev/ttyACM0 -b 921600 write_flash \
  -fs 4MB -fm dio -ff 80m 0x2000 nuttx/vela_nuttx.bin
```

当前 RAM-only-header 镜像不附加 SHA-256 Digest，ROM 会打印 comparison failed
后继续启动；这与 DSI/触摸驱动无关。

## 7. 验收流程

### 7.1 显示

1. 全屏彩条稳定，无闪烁。
2. 无 DSI Timeout 或 EK79007 Command Error。
3. 串口出现 `EK79007 color bars active`。
4. 显示与触摸完整通过时 `ready=0000011d`。

### 7.2 触摸

执行 `tc 8`，按左上、右上、左下、右下依次短按并释放。实测值：

| 位置 | 坐标 |
| --- | --- |
| 左上 | `(5, 13)` |
| 右上 | `(1023, 6)` |
| 左下 | `(29, 594)` |
| 右下 | `(1021, 587)` |

再执行 `tc 20` 单指拖动，确认 DOWN、连续 MOVE、UP，且坐标变化方向正确。

## 8. 故障定位

### `EK79007 command 0xb2 failed: -110`

优先检查 DSI Host 的 Transfer Complete 路径。本次调试中，MIPI Message 未清零
导致写事务被误判为需要读取响应，最终等待超时。所有 Message 必须先零初始化，再
显式填写必要字段。随后检查 D-PHY LDO、Panel Reset、Lane 数量/速率和命令时序。

### 初始化成功但屏幕不亮

按顺序检查彩条启动返回值、DCS Display On、GPIO26 背光、Panel 3.3 V 和
D-PHY 2.5 V。背光必须最后开启。

### `0x5d/0x14` 均无 GT911

1. 检查 SCL/SDA 空闲电平。
2. 扫描总线；只发现 `0x18` 表示主板共享总线工作。
3. 检查 DSI 座到 Touch FPC 的 SCL/SDA 连通性和方向。
4. 将 INT_TP/RESET_TP 接到 GPIO4/GPIO5。
5. 执行标准复位选址时序。

### 可读 Product ID，但没有触摸

Reset 后必须将 INT 配置成浮空输入，不能启用内部上拉。确认按下时 `0x814e`
由 `0x00` 变为 `0x81`。

### 坐标方向错误

先采集带位置标签的四角数据。如果某轴两端反向，只修改该轴 Mirror；如果水平移动
改变 Y，则修改 Swap XY。不要直接照搬参考 BSP 的屏幕安装方向。

## 9. 本次 Bring-up 经验

1. 先用硬件彩条证明 DSI/Panel，再接 Framebuffer 和 GUI。
2. 显示与触摸是两条独立链路，即使它们共享连接器和 I2C。
3. I2C 扫描只能证明应答设备所在的总线段。
4. 能读 Product ID 不代表触摸扫描和 INT 已正常工作。
5. 调坐标前先保证 `maxpoint`、DOWN/MOVE/UP 和缓存尺寸正确。
6. 使用带标签的四角实测推导旋转，不复制参考板机械方向。
