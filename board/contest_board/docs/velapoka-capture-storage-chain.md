# VelaPoka 图像采集、检测与 SD 留档链路

本文按当前产品固件的实际代码，整理从 SC2336 出帧到 LVGL 预览、P0 检测及
MicroSD 持久化的完整数据链路。产品配置为 `configs/velapoka`，设备节点为
`/dev/video0`、`/dev/fb0`、`/dev/input0` 和 `/dev/mmcsd0`。

## 1. 总体数据流

```text
控制面
GT911 触摸
    │ 录入 / 检测 / 停止 / 退出
    ▼
LVGL/UI 主线程（VelaPoka 主任务，优先级 100）

图像数据面
SC2336
  1280×720、BGGR packed RAW10、30 fps、MIPI CSI-2 2 lane
    │
    ▼
ESP32-P4 CSI bridge（HSync framing）
    │
    ▼
ISP input route（RAW10 bypass，不做 ISP 图像处理）
    │
    ▼
DW-GDMA
    │
    ▼
NuttX imgdata/imgsensor lower-half
    │ capture_register("/dev/video0")
    ▼
NuttX V4L2 upper-half
    │ 2 × USERPTR RAW10 buffer，FIFO
    ▼
Camera 线程（优先级 110）
    │ poll → DQBUF → RAW10 高 8 位抽取 → 最近邻缩放 → QBUF
    ▼
2 × 512×288 L8 预览缓冲（只保留最新帧）
    ├──────────────────────────────┐
    │                              │
    ▼                              ▼
UI 主线程                       录入/检测路径
512×288 L8                    512×288 L8
    │                              │ 最近邻缩放
    │ 缩放 + 灰度转 RGB565         ▼
    ▼                          320×180 L8
460×259 RGB565                   ├─ 录入：连续 3 帧平均 → reference
    │                            └─ 检测：亮度归一化、块差、边缘差
    ▼                                      │
LVGL Image + ROI + 差异框                    ▼
/dev/fb0 → MIPI-DSI                    PASS/FAIL + 得分 + 差异框
                                               │
                         ┌─────────────────────┴────────────────────┐
                         ▼                                          ▼
                    LVGL 结果显示                           Storage 队列（深度 4）
                                                                    │
                                                                    ▼
                                                        Storage 线程（优先级 90）
                                                                    │
                                                                    ▼
                                                        FAT32 /dev/mmcsd0
```

链路中有三个独立执行上下文：Camera 线程持续采集和生成最新预览；Inspect
线程只处理冻结后的单帧；Storage 线程负责所有文件 I/O。LVGL API 只由 UI 主线程
调用，SD 写入不会直接阻塞相机线程。

## 2. 板级初始化与设备注册

### 2.1 SC2336 控制面

- SC2336 通过共享软件 I2C 控制，地址 `0x30`，总线频率 100 kHz。
- 初始化首先读取 `0x3107/0x3108`，期望 PID 为 `0xcb3a`。
- CSI D-PHY 使用 LDO3 2.5 V；它与 DSI 共用电源通道，通过引用计数共享。
- 写入 1280×720、30 fps 的传感器寄存器表后，将 `0x0100` 写为 `0x00`，保持
  standby。只有 V4L2 开始采集后才将其切到 `0x01`。
- stream on/off 均执行“写寄存器、等待 5 ms、读回校验”，最多重试 3 次。

### 2.2 CSI 数据面

当前固定格式如下：

| 项目 | 配置 |
| --- | --- |
| 传感器输出 | 1280×720 BGGR packed RAW10 |
| 帧率 | 30 fps |
| CSI lane | 2 |
| lane bit rate | 405 Mbps |
| RAW 帧大小 | 1,152,000 B（1280×720×5/4） |
| DMA 对齐 | 64 B |
| CSI transaction queue | 1 |
| ISP | 仅配置 CSI 输入路由，RAW10 bypass |

CSI bridge 必须启用 HSync framing。当前传感器模式不依赖 CSI-2 line-start/
line-end short packet；关闭 HSync 会导致 bridge 在整帧 DMA 完成前丢弃帧。

CSI 和 DSI 共用 MIPI/DW-GDMA 时钟域。板级初始化先创建并保留 CSI controller，
再初始化显示；关闭 `/dev/video0` 时只停止接收，不销毁 CSI/ISP/DMA reservation，
避免下次打开相机时复位正在扫描的 DSI 通道。

板级驱动通过：

```text
capture_register("/dev/video0", &imgdata, &imgsensor, 1)
```

把 SC2336 sensor lower-half 和 ESP32-P4 CSI imgdata lower-half 接到 NuttX V4L2
upper-half，最终向应用提供 `/dev/video0`。

## 3. DMA 到 V4L2 的缓冲流转

应用打开 `/dev/video0` 后按以下顺序配置：

```text
open
  → VIDIOC_S_FMT     1280×720 / V4L2_PIX_FMT_SBGGR10P
  → VIDIOC_S_PARM    1/30 s
  → VIDIOC_REQBUFS   2 × USERPTR / FIFO
  → memalign(64)     分配两个 1,152,000 B RAW buffer
  → VIDIOC_QBUF      把两个 buffer 交给 V4L2
  → VIDIOC_STREAMON
```

每个 RAW buffer 的生命周期为：

```text
应用 QBUF
  → imgdata.set_buf：DMA 前 cache invalidate，发布 next_buffer
  → CSI on_get_new_trans：取走 next_buffer
  → 没有可用 USERPTR 时写入驱动 scratch buffer，保持 CSI 连续流动
  → DW-GDMA 写满一帧
  → CSI on_trans_finished：只记录完成信息并投递 LPWORK
  → LPWORK 调用 V4L2 complete_capture
  → poll(POLLIN) 唤醒 Camera 线程
  → VIDIOC_DQBUF
  → 应用读取并转换 RAW 数据
  → VIDIOC_QBUF，进入下一轮
```

驱动另外分配一个 1,152,000 B、64 B 对齐的 scratch buffer。当 V4L2 尚未及时
提交下一块 USERPTR 时，CSI 把该帧写入 scratch 并丢弃，不停止 sensor，也不让
DMA 队列因短暂背压卡住。

CSI 完成回调运行在 DMA/IRQ 路径，只做计数和投递工作；V4L2 完成通知放到
LPWORK。这样避免在 2 KiB HPWORK 栈上进入较深的 V4L2 buffer manager，同时降低
共享 DW-GDMA 中断被阻塞的风险。

## 4. RAW10 到实时预览

Camera 线程使用 `poll()` 等待帧，单次超时 1 s，连续 5 次超时后将相机标记为
错误。DQBUF 后检查 buffer index、USERPTR、`bytesused == 1,152,000` 及 error flag。

SC2336 packed RAW10 每 4 个像素占 5 B。当前预览只取前 4 B 中每个 Bayer sample
的高 8 位，忽略第 5 B 中打包的低 2 位：

```text
raw[(x / 4) × 5 + (x % 4)]
```

随后执行：

1. 1280×720 以最近邻方式缩放到 512×288。
2. 高 8 位乘 `CONFIG_EXAMPLES_VELAPOKA_PREVIEW_GAIN`，当前默认 4。
3. 超过 255 的值饱和为 255，得到 8-bit 灰度 L8 帧。
4. RAW buffer 立即 QBUF 归还 V4L2。

预览使用两个 512×288（147,456 B）缓冲，状态为 `FREE → WRITING → READY →
READING → FREE`。Camera 线程发布新帧时会释放尚未消费的旧 READY 帧，因此系统
采用“最新帧优先”，不会因 UI 慢于 30 fps 而积累延迟。

UI 主线程通过 `velapoka_camera_acquire()` 取得最新帧，并在同一次持有期间完成
预览更新、录入或检测提交；随后调用 `velapoka_camera_release()`。Inspect 模块会把
所需内容复制到自己的 320×180 缓冲，因此释放预览帧后不会继续引用它。

## 5. LVGL 显示支路

UI 将 512×288 L8 再以最近邻方式缩放到 460×259，并通过 256 项查表转换为
RGB565。RGB565 图像作为 LVGL Image 数据源，调用 invalidate 后在下一轮 LVGL
刷新中写入双 framebuffer，最终经 `/dev/fb0` 输出到 1024×600 MIPI-DSI 屏幕。

UI 同时维护：

- 绿色固定 ROI 框；
- 实时 FPS；
- PASS/FAIL、相似度、差异比例和检测耗时；
- 最多 8 个红色差异框。

检测坐标属于 320×180 空间。显示时按比例映射到 460×259，不改变检测结果中的
原始坐标。

## 6. 标准件录入链路

点击“录入”后状态机进入：

```text
READY/IDLE → SAMPLE_CAPTURE → SAVE_SAMPLE → READY
```

每收到一张新的 512×288 预览帧：

1. 最近邻缩放为 320×180 L8。
2. 连续采集 3 帧，对每个像素做 `uint16_t` 累加。
3. 第 3 帧后逐像素除以 3，生成 57,600 B reference。
4. reference 立即在内存中生效。
5. 将 reference 和当前面积阈值复制到 Storage 线程，异步写入 SD。

SD 上的模板文件为：

```text
/mnt/sdcard/velapoka/template.bin
```

它由 36 B 小端头和 57,600 B 灰度模板组成。头部包含魔数 `VKP1`、版本、图像
尺寸、ROI、阈值、payload 长度和 reference CRC32。写入过程使用
`template.tmp → fsync → close → rename(template.bin)`；阈值同时以相同的临时文件
替换方式写入 `config.json`。

下次启动时先挂载 SD、加载并校验 header/CRC，再导入 Inspector。模板有效时应用
由 `IDLE` 自动进入 `READY`，无需重新录入。

## 7. 单次检测链路

点击“检测”不会复用屏幕上已经显示的旧帧，而是把状态切到 `CAPTURE`，等待下一张
Camera 预览帧：

```text
READY → CAPTURE → PREPROCESS → INSPECT → PASS/FAIL
      → SAVE_RESULT → READY
```

收到下一帧后：

1. 将 512×288 L8 复制/缩放为 Inspector 自有的 320×180 `current`。
2. 记录对应 V4L2 `sequence` 和 UI 阈值。
3. 唤醒独立 Inspect 线程，UI 和 Camera 继续运行。
4. 在固定 ROI `(48,32,224,112)` 内计算 reference/current 平均亮度。
5. 用 Q12 增益把 current 归一化到 reference 亮度，增益上限为 8 倍。
6. ROI 划分为 14×7 个 16×16 block，共 98 块。
7. 每块计算灰度绝对差与水平/垂直边缘差，组合指标为
   `3/4 × 灰度差 + 1/4 × 边缘差`。
8. 指标达到满量程的 12% 时将该块标为异常。
9. 对异常块做 8 邻域连通域合并，按面积排序并输出最多 8 个框。
10. `difference = 异常块数 / 98`，`score = 100% - difference`。
11. 总异常比例和最大连通区域同时满足阈值才判定 PASS。

Inspector 线程完成后，UI 主线程轮询取走结果，更新状态、指标和红框。

## 8. 检测结果写入 SD

### 8.1 入队不等于落盘

`velapoka_storage_save_result()` 会先完成以下内存操作：

- 分配严格递增的 record ID；
- 复制 `velapoka_result_s`；
- FAIL 时复制 57,600 B normalized 灰度图；
- 更新最近 3 条历史；
- 放入深度为 4 的环形队列并唤醒 Storage 线程。

串口日志：

```text
velapoka: result queued as record #N
```

只表示数据已经安全复制进内存队列，并不表示 SD 已完成写入。队列满时返回
`-EBUSY`，不会覆盖尚未写入的旧记录。

### 8.2 Storage 线程落盘顺序

Storage 线程以优先级 90 运行，模板请求优先于结果请求。单条检测记录按以下顺序
写入：

```text
FAIL：fail/fail_NNNNNNNN.bmp → config.json → results.jsonl append
PASS：                         config.json → results.jsonl append
```

每次文件写入都处理短写和 `EINTR`，完成后执行 `fsync()` 再 `close()`。如果任何
I/O 失败，Storage 状态变为 offline 并记录负 errno；本地相机和检测链路仍可继续，
但后续留档会返回错误。

### 8.3 FAIL BMP

FAIL 图像保存为：

```text
/mnt/sdcard/velapoka/fail/fail_NNNNNNNN.bmp
```

格式为 320×180、8-bit 灰度 BMP：54 B BMP/DIB 头、1024 B 灰度调色板和
57,600 B 像素，总大小 58,678 B。BMP 采用 bottom-up 行序；保存的是完成亮度
归一化的检测帧，而不是 1280×720 RAW10 原图。差异框不烧录进 BMP 像素，而是
以坐标写入同一条 JSONL 记录。

### 8.4 results.jsonl

每个结果追加一行到：

```text
/mnt/sdcard/velapoka/results.jsonl
```

字段包括：

```json
{"id":10,"frame":3490,"result":"PASS","score_tenths":1000,
 "difference_tenths":0,"elapsed_ms":10,"threshold_percent":18,
 "image":"","boxes":[]}
```

FAIL 记录的 `image` 指向相对路径，`boxes` 保存 320×180 检测坐标。启动时按文件
顺序读取历史；重复或倒序 ID 会计数并跳过，下一编号从最大有效 ID 加 1。

## 9. 启动、运行和安全退出时序

### 9.1 启动

```text
板级 bring-up
  → 注册 /dev/video0（此时 sensor standby）
  → 初始化 /dev/fb0
  → 注册 /dev/mmcsd0
  → 自动创建 VelaPoka 主任务

VelaPoka 主任务
  → 初始化 LVGL
  → 启动 Inspector
  → 挂载 FAT、加载 history/template/config、启动 Storage 线程
  → 打开 /dev/video0、提交 RAW buffers、STREAMON
  → sensor 0x0100 = 0x01
  → 开始实时预览
```

存储恢复先于相机开流，因此模板、阈值、历史和 next ID 在第一张检测帧到来前已经
确定。

### 9.2 安全退出

触屏 Exit 后按以下顺序清理：

```text
停止 Camera 线程
  → VIDIOC_STREAMOFF
  → 停止 CSI controller
  → sensor 0x0100 = 0x00
  → close(/dev/video0)

停止 Inspect 线程并 join

通知 Storage 停止
  → 继续处理 reference_pending 和全部 result queue
  → 队列为空后退出并 join
  → umount(/mnt/sdcard)（仅当本应用执行过 mount）

显示 SAFE EXIT COMPLETE
  → 返回 NSH
```

只有看到以下日志，才可以把“本轮数据已持久化并可安全拔卡”视为成立：

```text
VelaPoka storage: unmounted /mnt/sdcard
velapoka: safe shutdown complete
nsh>
```

## 10. 主要缓冲与所有权

| 缓冲 | 数量 × 单块大小 | 写入方 | 读取方 | 释放/复用条件 |
| --- | ---: | --- | --- | --- |
| CSI scratch RAW10 | 1 × 1,152,000 B | DW-GDMA | 不读取 | 无 USERPTR 时循环覆盖 |
| V4L2 USERPTR RAW10 | 2 × 1,152,000 B | DW-GDMA | Camera 线程 | 转换后立即 QBUF |
| Camera L8 preview | 2 × 147,456 B | Camera 线程 | UI 主线程 | acquire/release；旧 READY 可丢弃 |
| UI RGB565 preview | 1 × 238,280 B | UI 主线程 | LVGL | 每个新 sequence 更新 |
| Inspector reference | 1 × 57,600 B | 录入/恢复 | Inspect 线程 | 重新录入或退出 |
| Inspector current | 1 × 57,600 B | UI 提交路径 | Inspect 线程 | 单次任务完成后复用 |
| Inspector normalized | 1 × 57,600 B | Inspect 线程 | UI/Storage snapshot | 结果入队复制后复用 |
| Storage result image | 4 × 57,600 B | UI 主线程复制 | Storage 线程 | 对应请求落盘后复用 |

## 11. 关键观测点

| 层级 | 正常观测 |
| --- | --- |
| 板级 probe | `SC2336: PID=0xcb3a ... /dev/video0 registered` |
| CSI 配置 | `SC2336 CSI: RAW10 bypass, bridge HSync framing enabled` |
| sensor 开流 | `SC2336: stream on, reg 0x0100=0x01` |
| 应用采集 | `velapoka: live preview ready ... camera=/dev/video0` |
| 检测 | `result=PASS/FAIL ... sequence=N` |
| 留档入队 | `result queued as record #N` |
| sensor 关流 | `SC2336: stream off, reg 0x0100=0x00` |
| 数据持久化完成 | `VelaPoka storage: unmounted /mnt/sdcard` |
| 全流程退出 | `velapoka: safe shutdown complete` |

## 12. 对应代码

- `board/contest_board/include/board.h`：相机、电源和总线参数。
- `board/contest_board/src/velapoka_bsp.c`：板级初始化顺序。
- `board/contest_board/src/velapoka_sc2336.c`：SC2336、CSI/imgdata lower-half 和
  `/dev/video0` 注册。
- `app/velapoka/velapoka_camera.c`：V4L2 USERPTR、采集线程、RAW10 转 L8 和最新帧
  双缓冲。
- `app/velapoka/velapoka_main.c`：UI、Camera、Inspect、Storage 的生命周期和状态机。
- `app/velapoka/velapoka_inspect.c`：录入、缩放、归一化、块差和差异框。
- `app/velapoka/velapoka_ui.c`：L8 转 RGB565、LVGL 预览和结果叠加。
- `app/velapoka/velapoka_storage.c`：FAT 挂载、后台队列、模板、BMP、JSONL、fsync
  和安全卸载。
