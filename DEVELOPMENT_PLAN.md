# VelaPoka 比赛开发计划与进度

> 本文档是 VelaPoka 比赛作品开发的唯一计划与进度事实源。
> 开启新会话后，应先阅读本文档，再查看 `git status` 和相关实板日志。
> 完成与比赛功能有关的工作后，应同步更新“当前进度”“下一步”和“变更记录”。

- 最后更新：2026-09-06
- 当前阶段：核心功能与 RJ45 导出已通过实板验收并归档；功能范围冻结，进入演示与最终提交整理
- 比赛截止：2026-09-20
- 功能冻结目标：2026-09-18

## 1. 作品目标

在 ESP32-P4 Function EV Board 和 openvela/NuttX 上实现一个可离线运行、
可复现构建、可现场演示的“视觉装配防错终端”：

```text
SC2336 相机采集
       ↓
灰度预处理、ROI、模板差和边缘差
       ↓
PASS / FAIL、得分、差异区域
       ├── LVGL + MIPI-DSI 实时显示
       ├── GT911 触摸操作
       ├── MicroSD 样本和结果留档
       └── Ethernet HTTP 查询与导出（比赛项目描述承诺）
```

P0 目标是形成“录入标准件 → 拍摄 → 检测 → 显示 → 留档”的完整闭环。
不依赖互联网，不要求彩色预览；稳定的灰度预览更适合当前确定性视觉算法。

## 2. 新会话接手规则

1. 先阅读本文档，确认当前阶段和“下一步具体任务”。
2. 执行 `git status`，保留用户已有修改，不覆盖无关工作。
3. 板级源码以 `board/contest_board/` 为维护源；工作树中的
   `vendor/openvela/boards/contest2026_284_board/` 可能与其为 hard link。
4. 产品配置使用 `configs/velapoka`，`configs/nsh` 始终保留为最小恢复配置。
5. 外设只有在“设备节点存在 + 实板 smoke test 通过”后才能标记为完成。
6. 结束比赛功能任务前，更新本文档的状态矩阵、下一步和变更记录。

## 3. 当前实板基线

| 模块 | 当前状态 | 实板证据 | 剩余工作 |
| --- | --- | --- | --- |
| UART/NSH | 已验证 | 可进入 NSH，系统时间正常运行 | 保持恢复配置稳定 |
| PSRAM | 已验证 | 识别 32 MiB，相机和 framebuffer 可分配 | 演示前确认正常启动 |
| GT911 | 已验证 | `/dev/input0`，四角及 DOWN/MOVE/UP 已通过 | 演示录入、检测、停止和退出 |
| MIPI-DSI | 双缓冲实板验收通过 | `/dev/fb0`，1024×600 RGB565；双 framebuffer、后备缓冲缓存同步和 VSync 切换确认无撕裂 | 演示前确认画面正常 |
| SC2336/CSI | smoke test 已通过 | 两次 `camtest preview 3 5000` 均 PASS，完整 1152000 字节帧 | 演示实时预览和检测 |
| 相机预览 | M2 实板验收通过 | 512×288 灰度预览稳定 15 fps；下一 VSync 绘制门控确认无撕裂 | 纳入演示脚本 |
| MicroSD | M4 归档通过 | SPI2 `M4HW` 经卸载、断电后 Windows 可读；模板、JSONL、配置和 FAIL BMP 可见；触屏 Exit 排空队列并卸载 FAT | 演示 FAIL BMP 与 JSONL 导出 |
| Ethernet | M5 核心归档通过 | `eth0=10.0.0.2/24` 自动配置；状态、完整 JSONL、FAIL BMP、路径保护、网络负载下本地检测和断线恢复实板通过 | 纳入演示脚本；图形化网页为可选增强 |
| Wi-Fi | 驱动层实板验收通过 | ESP32-C6 SDIO 4-bit/20 MHz、ESP-Hosted RPC、STA 启动、MAC 获取及 `wlan0` 注册通过；`ready=0x33f` 包含 Wi-Fi 位 | 后续如需联网功能，再以测试镜像验证扫描、关联、DHCP 和双向收发；产品暂不增加 Wi-Fi 应用 |
| LVGL | M2 双缓冲实板验收通过 | 实时 RGB565 Image 使用一次缩放和色彩转换；双 framebuffer/VSync 下稳定 15 fps、无撕裂 | 演示状态、结果和异常框 |
| 产品应用 | M4 归档通过 | 上电自动进入应用；`loaded=9 duplicates=1 next=10` 后生成 `#10` PASS；独立 Exit 停相机、卸载 FAT 并返回 NSH | 冻结演示操作流程 |
| AI/比赛日志 | 已有基础 | `logs/wx719/` 已归集部分会话 | 持续归集、脱敏、校验 manifest |

### 3.1 已确认的相机关键约束

- SC2336 输出格式为 1280×720、BGGR packed RAW10、30 fps。
- CSI bridge 必须保持 HSync framing 开启；正确寄存器值应包含
  `frame_cfg` bit 24，即历史成功值 `0x015002d0`。
- CSI 与 DSI 共享 MIPI/DW-GDMA 资源；显示应持续扫描，不应在相机采集时
  反复停止、销毁和重建 DSI channel。
- 共享中断适配补丁位于
  `board/contest_board/chips/esp32p4/patches/0004-nuttx-shared-interrupt-adapter.patch`。
- `camtest` 保留为驱动 smoke test，不能继续承载产品业务逻辑。

## 4. 产品界面方案

整个 1024×600 framebuffer 统一由 LVGL 管理。禁止 LVGL 和相机程序分别
直接写 `/dev/fb0`，否则 LVGL direct mode 的双缓冲会覆盖另一侧画面。

```text
┌──────────────────── 左侧 512×600 ────────────────────┬──────────── 右侧 512×600 ────────────┐
│ VelaPoka 视觉装配检测终端                              │ LIVE  10 FPS        产品 A-01       │
│                                                      │ ┌─────────────────────────────────┐ │
│ 系统状态：READY                                      │ │                                 │ │
│ Camera ●  SD ●  Ethernet ●                          │ │      512×288 灰度相机画面       │ │
│                                                      │ │      ROI 框 / 异常区域框         │ │
│ 当前样本：连接器标准件 A                              │ │                                 │ │
│ 检测阈值：18%  [────●────]                           │ └─────────────────────────────────┘ │
│                                                      │                                    │
│ [录入]   [检测]   [停止]   [退出]                     │ 识别结果：PASS / FAIL              │
│                                                      │ 相似度：96.4%                      │
│ ┌──────────────────────────────────────────────────┐ │ 差异占比：2.1%                     │
│ │ PASS                                             │ │ 检测耗时：132 ms                   │
│ │ 得分 96.4       耗时 132 ms                     │ │ 红框：漏装/错位区域                │
│ └──────────────────────────────────────────────────┘ │                                    │
│ 最近记录：#003 PASS  #002 FAIL  #001 PASS            │ [标准图缩略图] [差异图缩略图]       │
└──────────────────────────────────────────────────────┴────────────────────────────────────┘
```

右侧画面保持 16:9，使用 512×288，不拉伸为半屏高度。其余区域显示得分、
耗时、差异比例、标准图或差异图缩略图。

## 5. 软件架构与并发模型

```text
SC2336 /dev/video0
        │
        ▼
Camera 采集线程 ──→ 灰度缩放 ──→ 双预览缓冲 ──→ UI 消息队列
        │                                           │
        └─→ 冻结检测帧 ─→ Inspect 工作线程          ▼
                                │             LVGL UI 主线程
                                ├─→ 得分/差异框      │
                                └─→ Storage 队列     ▼
GT911 /dev/input0 ───────────────────────────→ /dev/fb0 → DSI
```

并发规则：

- 只有 UI 主线程可以调用 LVGL API。
- Camera 线程使用 V4L2 `poll → DQBUF → 处理 → QBUF`，尽快归还 RAW buffer。
- Camera 输入保持 30 fps；只向 UI 发布最新帧，预览刷新限制为 8～10 fps。
- 检测触发时复制/冻结一份缩小后的灰度图，不能长期占用 V4L2 buffer。
- Storage 工作线程负责 SD 卡 I/O，不允许阻塞 UI 或相机采集线程。
- UI 采用“最新结果优先”，消息积压时允许丢弃旧预览帧，不能堆积延迟。

## 6. P0 识别算法

第一版使用可解释、确定性、低算力的视觉算法：

1. 录入标准件时连续采集 3 帧，取平均生成参考模板。
2. 从 packed RAW10 提取高 8 位亮度，并缩放到 320×180。
3. 使用固定工装和固定 ROI。
4. 对当前图做平均亮度归一化，降低光照变化影响。
5. 将 ROI 划分为 16×16 或 20×20 像素块。
6. 计算模板绝对差和边缘差。
7. 对连续异常块进行合并，输出最多 8 个差异框。
8. 根据差异面积、最大异常区域和得分判断 PASS/FAIL。
9. 将差异框映射到右侧 512×288 预览画面。

后续 P1 可增加小范围平移校准、分产品阈值和失败类型；轻量神经网络属于
P2，不得影响 P0 闭环和稳定性。

## 7. 产品状态机

```text
STARTUP → SELF_TEST → IDLE
                       │
          录入标准件   ▼
              SAMPLE_CAPTURE → SAVE_SAMPLE → READY
                                               │
                                               ▼
CAPTURE → PREPROCESS → INSPECT → PASS/FAIL → SAVE_RESULT → READY
                              │
                              └──────────────→ ERROR
```

所有状态变化必须同时反映到左侧状态卡和串口日志。显示、相机、SD 卡中的
非关键模块失败时应提供可诊断错误，不应让 NSH 恢复入口失效。

## 8. 可交付目录规划

当前比赛 manifest 只映射了 `board/contest_board`。现有工作树中的
`apps/examples/camtest` 是未跟踪目录，尚不能随团队仓交付。应用开发前必须先
建立 manifest 映射：

```xml
<linkfile src="app/camtest" dest="apps/examples/camtest"/>
<linkfile src="app/velapoka" dest="apps/examples/velapoka"/>
```

建议的产品应用结构：

```text
app/velapoka/
├── Kconfig
├── Make.defs
├── Makefile
├── CMakeLists.txt
├── velapoka_main.c
├── velapoka_ui.c
├── velapoka_ui.h
├── velapoka_camera.c
├── velapoka_camera.h
├── velapoka_inspect.c
├── velapoka_inspect.h
├── velapoka_storage.c
├── velapoka_storage.h
├── velapoka_state.c
├── velapoka_state.h
└── velapoka_model.h
```

模块边界：

- `velapoka_main`：初始化、线程和总生命周期。
- `velapoka_ui`：LVGL 对象、事件回调和界面更新。
- `velapoka_camera`：V4L2、RAW10 解包、灰度缩放和预览双缓冲。
- `velapoka_inspect`：ROI、模板、差异分析、得分和异常框。
- `velapoka_storage`：样本、JSONL、BMP、目录和错误恢复。
- `velapoka_state`：产品状态机和跨模块消息。

## 9. LVGL 配置基线

产品 `configs/velapoka/defconfig` 计划启用：

```text
CONFIG_GRAPHICS_LVGL=y
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_CONF_MINIMAL=y
CONFIG_LV_USE_NUTTX=y
CONFIG_LV_USE_NUTTX_TOUCHSCREEN=y
CONFIG_LV_MEM_SIZE_KILOBYTES=256
CONFIG_LV_USE_LABEL=y
CONFIG_LV_USE_BUTTON=y
CONFIG_LV_USE_BAR=y
CONFIG_LV_USE_SLIDER=y
CONFIG_EXAMPLES_VELAPOKA=y
```

第一版仅启用 Label、Button、Image、Bar、Slider、List 等需要的控件。
LVGL 使用 NuttX fbdev direct mode。M2 已恢复两个 RGB565 framebuffer，
在后备缓冲绘制并通过 VSync 边界的 `FBIOPAN_DISPLAY` 切屏；`FBIO_UPDATE`
负责同步对应后备缓冲的脏矩形缓存。

## 10. 内存与性能预算

| 用途 | 预算 |
| --- | ---: |
| 1024×600 RGB565 framebuffer 双缓冲 | 2.46 MB |
| 1280×720 RAW10 相机双缓冲 | 2.30 MB |
| CSI scratch buffer | 1.15 MB |
| 512×288 L8 采集双缓冲 + RGB565 UI 显示缓冲 | 0.59 MB |
| 参考图、当前图、差异图 | 约 0.18 MB |
| LVGL heap | 0.25 MB |
| 预计主要工作集 | 约 7 MB，目标控制在 8 MB 内 |

性能目标：

- 传感器采集：30 fps，无超时。
- UI 预览：稳定 8～10 fps。
- 显示扫描：保持现有约 30 Hz。
- 单次 P0 检测：目标小于 500 ms，验收上限 1 s。

## 11. 里程碑与进度看板

### M0：交付结构和基线冻结（目标 8 月 29 日）

- [x] 将 `camtest` 移入团队仓 `app/camtest` 并增加 manifest linkfile。
- [x] 创建 `app/velapoka` 并增加 manifest linkfile。
- [x] 更新 README 中已过期的 CSI、framebuffer 和外设状态。
- [x] 确认 `nsh`、`velapoka` 两套配置均可构建。

退出条件：从团队仓 manifest 同步后能得到完整板级代码和应用源码。

### M1：LVGL 左右分屏 first-light（目标 8 月 30–31 日）

- [x] 启用 LVGL、NuttX fbdev 和 touchscreen port。
- [x] 创建 1024×600 左右两个 LVGL container。
- [x] 实现状态卡、按钮、结果卡和右侧预览占位图。
- [x] 验证 GT911 点击、滑块和按钮事件（按钮及阈值调节功能正常；单
      framebuffer 在快速点击时仍有可见撕裂，作为 M1 已知限制转入 M2）。

退出条件：实板显示静态分屏 UI，触摸连续操作 10 分钟正常。

### M2：LVGL 实时相机预览（目标 9 月 1–3 日）

- [x] 抽取 `camtest` 的 V4L2 采集逻辑为产品相机模块。
- [x] 实现 512×288 灰度预览双缓冲和最新帧优先发布。
- [x] 使用 LVGL Image 显示画面，增加 ROI 框和实时帧率标签。
- [x] 相机采集与 LVGL 双 framebuffer 同时运行，实板稳定 15 fps 且无撕裂。

归档结论：核心功能和显示质量通过实板验收，详见
`board/contest_board/docs/m2-camera-preview-archive.md`。长时间压力运行不纳入
最终比赛交付范围。

### M3：标准件录入与 P0 检测（目标 9 月 4–7 日）

- [x] 实现状态机和“录入标准件/开始检测/停止”按钮。
- [x] 实现三帧平均参考模板和固定 ROI。
- [x] 实现亮度归一化、块差、边缘差和异常框合并。
- [x] 显示 PASS/FAIL、得分、差异比例和检测耗时。

软件状态：320×180 模板和检测帧、224×112 固定 ROI、16×16 块、最多 8 个
连通异常框均已进入产品固件；检测在线程中运行，UI 主线程只消费结果。比赛交付
采用固定工位演示，三类样本批量标定与统计不再作为最终提交阻塞项。

退出条件：现场可完成标准件录入、PASS/FAIL 检测、异常框显示及结果留档演示。

### M4：MicroSD 留档和历史记录（目标 9 月 8–10 日）

- [x] 完成 `/dev/mmcsd0` 和 FAT 自动挂载实板验收。
- [x] 保存产品模板和配置（实板落盘及应用自动恢复通过）。
- [x] 按 JSONL 保存检测记录（断电恢复及 Windows 可见通过）。
- [x] FAIL 时保存灰度 BMP 和差异框信息（文件、大小和 JSON 坐标已验证；Windows
      视觉打开纳入演示流程）。
- [x] 左侧显示最近检测历史（编号恢复及续接通过）。

软件状态：应用启动时尝试将 `/dev/mmcsd0` 以 FAT 挂载到
`/mnt/sdcard`，模板使用带版本、尺寸、ROI 和 CRC32 的二进制头，配置使用
JSON；检测记录追加到 JSONL，FAIL 图像保存为 320×180 的 8-bit 灰度 BMP，
异常框保存在同一 JSONL 记录中。所有运行期写盘进入优先级 90 的独立线程，
结果队列深度为 4；UI 显示真实 SD 在线状态及最近 3 条记录。

产品固件在板级 bring-up 成功后直接创建 VelaPoka 任务。点击独立 `Exit` 按钮执行
安全退出，长按 Stop 保留为备用：主循环停止相机和检测生产者后，存储线程排空
模板及结果队列，再卸载由应用挂载的 FAT；屏幕最终显示 `SAFE EXIT COMPLETE`。
历史加载要求追加记录 ID 严格递增，重复或倒序旧记录会计数并跳过，下一编号从
最大有效 ID 继续。

归档结论：模板和历史可跨断电恢复，应用数据在 Windows 可见，开机自动进入应用，
独立 Exit 已实板确认停止相机、排空存储、卸载 FAT 并返回 NSH。M4 核心退出条件
通过，详见 `board/contest_board/docs/m4-storage-archive.md`；PC 打开 FAIL BMP 纳入
最终演示流程。

### M5：网络导出与工程增强（目标 9 月 11–14 日）

- [x] 产品启动时自动为 `eth0` 配置固定静态 IPv4。
- [x] 实现 `/api/status`、`/api/results` 和 FAIL BMP 下载。
- [x] 网络断开不影响本地检测。

归档状态：新增优先级 85、栈 6144 字节的只读 HTTP 线程，监听
`10.0.0.2:8080`。HTTP 线程只接受 `GET`，导出路径限制为 `results.jsonl` 和
`fail/fail_XXXXXXXX.bmp`；SD 文件在存储 I/O 锁内复制为内存快照，发送阶段不占用
写盘锁，慢客户端不会直接阻塞检测留档。触屏 Exit 先停止网络线程，再停止相机、
排空存储队列并卸载 FAT。静态 IP、状态、完整 JSONL、58678 B FAIL BMP、非 GET
拒绝、路径白名单、网络请求期间本地检测和重新连线后的自动恢复均已实板通过，
详见 `board/contest_board/docs/m5-ethernet-export-archive.md`。M5 核心查询与导出归档
通过；heap、任务栈和额外性能统计不再作为比赛最终提交阻塞项。

### M6：文档和演示冻结（目标 9 月 15–18 日）

- [ ] Windows 打开最新 FAIL BMP，并核对最近 3 条屏幕历史与 JSONL 一致。
- [ ] 更新构建、烧录、操作、故障排查和架构文档。
- [ ] 归集并脱敏 AI Coding 日志。
- [ ] 完成 5 分钟内演示脚本和视频。

## 12. 当前下一步

M5 RJ45 核心查询与导出已于 2026-09-06 完成实板验收并归档，比赛功能范围冻结：

1. 编写并实走一遍 5 分钟内演示脚本，覆盖开机、录入、PASS/FAIL、SD 留档、
   RJ45 查询/导出和触屏安全退出。
2. 录制演示视频并核对画面、串口和 Windows 导出结果清晰可见。
3. 复核构建、烧录、操作和故障排查文档，不再增加非演示必需功能。
4. 归集、脱敏并校验 AI Coding 日志，提交并推送最终比赛版本。

## 13. 构建与验收命令

在 openvela 工作区根目录执行：

```bash
./build.sh vendor/openvela/boards/contest2026_284_board/configs/nsh -j2
./build.sh vendor/openvela/boards/contest2026_284_board/configs/velapoka -j2
```

相机回归：

```text
camtest preview 3 5000
camtest 100 5000
```

产品应用计划命令：

```text
velapoka
```

每个里程碑必须保留构建结果、串口日志和实板可观测结果。

## 14. 变更记录

| 日期 | 变更 | 结果 |
| --- | --- | --- |
| 2026-08-28 | 修复共享 IRQ、预览双缓冲和 CSI bridge HSync 配置 | 相机连续完整抓帧，屏幕正常显示灰度画面 |
| 2026-08-28 | 确定 1024×600 左右分屏产品方案 | 左侧 LVGL 业务 UI，右侧画面、ROI、差异框和结果 |
| 2026-08-28 | 建立本计划与进度事实源 | 后续会话以本文档作为接手入口 |
| 2026-08-28 | 完成 M1 LVGL first-light 代码和固件构建 | `velapoka` 命令、静态左右分屏、按钮、滑块和 GT911 port 已进入固件，待实板 10 分钟验收 |
| 2026-08-28 | 修复 M1 点击反馈慢 | first-light 改为单 framebuffer + `FBIO_UPDATE` 脏矩形刷新，避免小控件更新等待双缓冲切屏及整帧 cache 同步 |
| 2026-08-29 | 修复 GT911 短按漏采样 | 轮询周期由 10 tick（100 ms）降为 1 tick（10 ms），并增加 DOWN/UP 坐标日志用于实板验收 |
| 2026-08-29 | 修复 GT911 与画面坐标不一致 | 实板日志确认触摸相对 framebuffer 旋转 180°，驱动同时反转 X/Y 后再上报 LVGL |
| 2026-08-29 | 收敛 M1 UI 实板问题 | 启动前强制同步黑色 framebuffer，约束状态文字宽度，并为阈值增加拖动提示及 −/+ 微调按钮 |
| 2026-08-29 | 提升 M1 高频交互显示稳定性 | 加粗阈值滑块并强化进度对比；将大脏矩形拆为 8 行缓存同步块，降低快速点击时 DSI/PSRAM 争用导致的黑条风险 |
| 2026-08-29 | 归档 M1 LVGL first-light 基线 | 左右分屏、GT911、按钮和阈值功能通过实板验收；记录快速点击仍有撕裂但不影响功能，转入 M2 双 framebuffer/VSync 解决 |
| 2026-08-29 | 闭环 M0 应用交付结构 | `camtest` 迁入 `app/camtest`，增加 manifest linkfile，并更新根目录和板级 README 的外设状态 |
| 2026-08-29 | 回归两套配置构建 | 固定 HAL `8d0a898` 加兼容补丁下，`nsh`、`velapoka` 均生成 `vela_nuttx.bin`；产品 ELF 含 `camtest`、`velapoka` |
| 2026-08-29 | 完成 M2 实时预览软件链路 | 新增产品 V4L2 线程、RAW10 灰度缩放、最新帧优先双缓冲和 LVGL RGB565 Image；产品固件构建通过，待实板验收及双 framebuffer/VSync |
| 2026-08-29 | 修复 M2 预览区域闪烁无画面 | 确认 LVGL 软件渲染不支持 L8 到 RGB565 的缩放/混色；UI 增加灰度查找表并转换为 RGB565 Image |
| 2026-08-29 | 优化 M2 帧率并消除撕裂 | 灰度转 RGB565 与 460×259 缩放合并为单次查表，取消 LVGL 二次软件缩放；恢复双 framebuffer 并在 VSync 边界切屏 |
| 2026-08-29 | 双 framebuffer 实板验收并解除预览限流 | 实板确认 5 fps 画面无撕裂；移除每 3 帧发布 1 帧的固定限流，改由最新帧优先双缓冲按消费能力自然丢帧 |
| 2026-08-29 | 修复 15 fps 下双缓冲抢写 | 实板确认解除限流后达到 15 fps 但再次撕裂；应用绘制前等待下一真实 VSync，驱动清除历史 VSync 信号后再阻塞，避免 LVGL 写入尚在扫描的缓冲 |
| 2026-08-29 | 归档 M2 实时相机预览 | 实板确认 512×288 灰度预览稳定 15 fps 且无撕裂；达到预览性能目标，长时间与检测并发压力测试转入 M6，开始 M3 P0 检测 |
| 2026-08-30 | 完成 M3 P0 检测软件链路 | 接入产品状态机、三帧平均模板、亮度归一化、块差/边缘差、连通异常框和独立检测线程；结果与最多 8 个红框进入 LVGL，产品固件构建通过，待实板标定与三类样本统计 |
| 2026-08-30 | 完成 M4 MicroSD 留档软件链路 | 接入 FAT 自动挂载、模板 CRC/配置恢复、JSONL、FAIL 灰度 BMP、4 深度后台队列和最近历史；产品固件生成 `vela_nuttx.bin`，待实板挂载与断电恢复验收 |
| 2026-08-30 | 修复 M4 首轮实板启动蓝屏 | ELF 将异常定位到 `mmcsd_lock()`；SC2336 `imgdata` 非首成员却被直接强转为父结构，错位写覆盖相邻 MMC/SD 槽。改为按 `offsetof` 还原父结构并重新构建固件，待实板复测 |
| 2026-08-30 | 修复 M4 SD 红灯与重复挂载回归 | 实板手工挂载证明设备和 FAT 正常；应用改用 `statfs()` 识别并复用已有 FAT 挂载，记录自身挂载所有权用于失败清理，将存储初始化前移到相机开流前，并增加分阶段错误日志；21:11 固件构建及 checkpatch 通过 |
| 2026-08-30 | 修复 M4 历史加载 `-EINVAL` | 分阶段日志确认挂载已成功；根因是产品配置仅支持 FAT 8.3，而 `results.jsonl`、`config.json` 和 FAIL BMP 使用长文件名。启用 `CONFIG_FAT_LFN`，显式限制 `CONFIG_FAT_MAXFNAME=32`，并增加应用编译期配置守卫；21:22 固件构建及 checkpatch 通过 |
| 2026-08-30 | M4 自动挂载与在线留档队列实板通过 | 启动日志确认存储先于相机开流且无错误；完成一次三帧模板录入和两次 FAIL 检测，耗时均为 10 ms，结果连续排队为记录 `#1`、`#2`；待冷启动核对文件和恢复状态 |
| 2026-08-30 | M4 FAT 文件落盘实板通过 | 复位后核对 `template.bin` 57636 B、`config.json` 108 B、`results.jsonl` 446 B 和两张 58678 B FAIL BMP；JSONL 的编号、帧号、得分、图像路径和差异框一致，模板头尺寸、ROI、阈值、负载长度及 CRC 字段正确 |
| 2026-08-30 | M4 应用启动恢复实板通过 | 手工预挂载 FAT 后启动应用，成功复用 `/mnt/sdcard` 且不再出现 `-20`/`-22`；历史加载无错误，状态由 `IDLE` 自动进入 `READY`，确认模板和阈值已恢复，待一次检测确认记录从 `#3` 续接 |
| 2026-08-30 | 修复 M4 重启后记录号回到 `#1` | 实板复验暴露历史记录被静默跳过；根因是产品 libc 未启用 `CONFIG_LIBC_SCANSET`，但 JSONL 结果和配置阈值解析使用了 `%[...]`。改为标准定宽字符串及空白数字解析，增加 `loaded/skipped/next` 启动日志；产品固件构建及 checkpatch 通过，SHA-256 为 `2c08568c6de5501ff28d206309dd9b64d8bebe0c6d720bd76352df1d9d15cf81` |
| 2026-08-30 | M4 历史编号续接修复实板通过 | 修复版在恢复已有 SD 数据后连续完成两次 FAIL 检测，耗时均为 10 ms，后台队列记录依次为 `#3`、`#4`；确认最大编号恢复及运行期递增正常，剩余 PC 解析和三次冷启动验收 |
| 2026-08-30 | M4 首次冷启动恢复实板通过 | 完整重启后无需重新录入即可检测，记录从已有 `#4` 正确续接为 `#5` 并继续增长到 `#6`，两次检测均为 10 ms；三次冷启动验收完成 1/3 |
| 2026-08-31 | 定位 M4 文件未跨设备持久化并构建底层修复候选 | Windows/Linux 均只能看到空目录，且 `chkdsk` 未发现 FAT 错误；最小 `M4TEST` 在板端写后可读、`umount` 并移卡后消失，排除 VelaPoka 文件格式。MMC/SD SPI 现检查写后 busy 等待结果和 CMD13 状态，产品配置降至 1 MHz 并启用 2 次重试；`vela_nuttx.bin` 构建及补丁/checkpatch 校验通过，待实板验证。 |
| 2026-08-31 | M4 写完成校验修复候选实板失败 | 新固件下最小 `M4FIX` 在板端写入、读回和卸载均成功，但完全断电移卡后 Windows 仍找不到文件，且没有新增的 busy/CMD13 错误；下一步先做 Windows 独立写入持久化测试，排除 SD 卡伪只读/故障保护，再决定继续修 GPIO SPI 或替换下半部。 |
| 2026-08-31 | M4 改用 ESP32-P4 SPI2 并通过跨设备持久化验收 | 适配 P4 当前 GPSPI HAL、GPIO Matrix 和时钟控制，以 SPI2 软件 CS 驱动 GPIO42/43/44/39；`M4HW` 经板端 `umount`、完全断电和 Windows 读取成功，定位原 GPIO 模拟 SPI 为根因。 |
| 2026-08-31 | M4 应用持久化闭环实板通过 | 应用恢复 7 条历史和模板后完成 `#7` PASS；复位后 JSONL 含完整记录，模板、配置和 FAIL 目录在 Windows 可见；清理大小写重复目录后 PC 与板端视图一致。 |
| 2026-08-31 | 构建 M4 安全退出收尾候选 | 长按 Stop 可退出主循环；退出先停止相机/检测线程，存储线程排空待写模板和结果后自动卸载自身挂载的 FAT。历史加载新增重复/倒序 ID 计数并从最大有效 ID 续号；产品固件构建及 checkpatch 通过。 |
| 2026-08-31 | M4 长按安全退出实板通过 | 启动识别 1 条重复旧 ID 并从 `#8` 续号，连续生成 `#8`、`#9` FAIL 后长按 Stop；串口确认相机 stream off、存储卸载、`safe shutdown complete` 并返回 `nsh>`，未出现存储错误。 |
| 2026-08-31 | 修复 M4 产品固件未自动进入应用 | 实板反馈 ROMFS 启动脚本未生效，改为板级 bring-up 成功后直接创建 `velapoka_main` 任务，优先级 100、栈 16 KiB，并保留 NSH 维护入口；机器码确认 `board_app_initialize()` 直接调用 `task_create()` 且入口地址为 `velapoka_main`。产品固件完整构建通过，SHA-256 为 `08dc9eae1e52e2b0f4cb40fb6d79fdc6af3f1786f132531c4339539e5f694b60`。 |
| 2026-08-31 | M4 自动启动与独立 Exit 实板通过并归档 | 无需命令自动进入应用，恢复日志为 `loaded=9 duplicates=1 skipped=0 next=10`；录入后生成 `#10` PASS，触屏 Exit 依次停止 CSI、关闭 sensor、卸载 FAT 并返回 NSH。M4 核心功能归档，BMP 视觉检查和最近历史顺序转入 M6。 |
| 2026-09-05 | 增加并完成板载 ESP32-C6 Wi-Fi 驱动适配 | 以 SDIO 4-bit/20 MHz 接入 ESP-Hosted 0.0.6；实板完成 CMD5 枚举、INIT event、Wi-Fi RPC、MAC 获取与 `wlan0` 注册，`ifconfig` 显示 MAC `10:bd:a3:8a:bc:55`，`/dev/velapoka` 返回 `ready=0x0000033f`。本阶段不新增 Wi-Fi 应用，扫描、关联和数据收发不计入验收。 |
| 2026-09-06 | 归档网口数据导出开发前基线 | 冻结已验证的显示、触摸、相机、P0 检测、MicroSD 留档、安全退出与 Ethernet/Wi-Fi 驱动边界；明确 M3 样本统计、RJ45 HTTP 查询/导出及 M6 压力验收仍未完成。 |
| 2026-09-06 | 完成 M5 RJ45 HTTP 导出软件候选 | 产品启动自动配置 `eth0=10.0.0.2/24` 并监听 8080；提供状态、JSONL 和严格白名单 FAIL BMP 下载，SD 快照读取与后台写盘串行、网络停止纳入安全退出；自检补齐 NuttX TCP backlog 和客户端超时强制检查，修复 ESP-Hosted 归档缺失 protobuf-c 子模块导致的干净构建失败，完整产品构建通过，待实板验收。 |
| 2026-09-06 | M5 RJ45 核心查询与导出实板通过并归档 | 静态 IP 自动启动和 0% Ping 丢包通过；状态 JSON、2775 B 完整 JSONL、58678 B FAIL BMP 下载以及 405/404 访问保护符合预期；连续网络请求期间本地录入、FAIL/PASS 和 SD 留档正常，重连后 HTTP 自动恢复并显示最新记录 `#17`，触屏 Exit 安全停止服务。前三项完成，工程指标仍待补齐。 |
| 2026-09-06 | BSP 收尾候选完成构建 | MicroSD 与 Ethernet 初始化成功后补齐 BSP ready 位；同步 README、架构和合规文档，移除误提交的依赖与备份文件；`nsh` 和 `velapoka` 全量构建通过，产品镜像 842260 B，待实板回归后提交归档。 |
| 2026-09-06 | 冻结最终比赛功能范围 | 三类批量标定、四步工序、触屏框选、定位标记校准、工程指标和长时间压力测试不再作为最终提交阻塞项；剩余工作收敛到演示脚本、视频、文档和日志。 |
