# VelaPoka 比赛开发计划与进度

> 本文档是 VelaPoka 比赛作品开发的唯一计划与进度事实源。
> 开启新会话后，应先阅读本文档，再查看 `git status` 和相关实板日志。
> 完成与比赛功能有关的工作后，应同步更新“当前进度”“下一步”和“变更记录”。

- 最后更新：2026-08-30
- 当前阶段：M3 P0 检测软件链路已构建，待实板标定与样本统计
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
       └── Ethernet HTTP 查询与导出（P1）
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
| PSRAM | 已验证 | 识别 32 MiB，相机和 framebuffer 可分配 | 增加并发压力和泄漏测试 |
| GT911 | 已验证 | `/dev/input0`，四角及 DOWN/MOVE/UP 已通过 | 实板验证 LVGL 按钮、滑块连续操作 10 分钟 |
| MIPI-DSI | 双缓冲实板验收通过 | `/dev/fb0`，1024×600 RGB565；双 framebuffer、后备缓冲缓存同步和 VSync 切换确认无撕裂 | 继续确认 30 分钟无蓝屏和队列卡死 |
| SC2336/CSI | smoke test 已通过 | 两次 `camtest preview 3 5000` 均 PASS，完整 1152000 字节帧 | 连续 100 帧和长时间并发测试 |
| 相机预览 | M2 实板验收通过 | 512×288 灰度预览稳定 15 fps；下一 VSync 绘制门控确认无撕裂 | M3 接入检测时回归帧率，长时间压力测试并入 M6 |
| MicroSD | 适配存在，未完成验收 | 配置和板级实现存在 | `/dev/mmcsd0`、FAT 挂载、CRC 读回 |
| Ethernet | 基础链路已验证 | `eth0` 静态 IPv4 双向 Ping | DHCP 回退和 HTTP 服务 |
| LVGL | M2 双缓冲实板验收通过 | 实时 RGB565 Image 使用一次缩放和色彩转换；双 framebuffer/VSync 下稳定 15 fps、无撕裂 | M3 叠加检测结果时回归刷新稳定性 |
| 产品应用 | M3 软件链路已构建 | 产品 ELF 含状态机、三帧标准模板、独立 P0 检测线程、结果指标和异常框 | 实板回归录入/检测状态流、阈值和预览帧率，完成三类样本统计 |
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
│ [录入标准件]   [开始检测]   [停止]                    │ 识别结果：PASS / FAIL              │
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
- 连续 100 次检测无明显内存递减。

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
`board/contest_board/docs/m2-camera-preview-archive.md`。30 分钟以上压力运行与
M3 检测并发回归合并到 M6 稳定性验收，不阻塞进入 M3。

### M3：标准件录入与 P0 检测（目标 9 月 4–7 日）

- [x] 实现状态机和“录入标准件/开始检测/停止”按钮。
- [x] 实现三帧平均参考模板和固定 ROI。
- [x] 实现亮度归一化、块差、边缘差和异常框合并。
- [x] 显示 PASS/FAIL、得分、差异比例和检测耗时。

软件状态：320×180 模板和检测帧、224×112 固定 ROI、16×16 块、最多 8 个
连通异常框均已进入产品固件；检测在线程中运行，UI 主线程只消费结果。以上项目
仍需实板标定和样本统计，尚未达到 M3 退出条件。

退出条件：正确、漏装、错位三类样本各完成不少于 20 次测试并统计结果。

### M4：MicroSD 留档和历史记录（目标 9 月 8–10 日）

- [ ] 完成 `/dev/mmcsd0` 和 FAT 挂载实板验收。
- [ ] 保存产品模板和配置。
- [ ] 按 JSONL 保存检测记录。
- [ ] FAIL 时保存灰度 BMP 和差异框信息。
- [ ] 左侧显示最近检测历史。

退出条件：断电重启后可读取模板和历史，文件在 PC 上可解析。

### M5：网络与工程增强（目标 9 月 11–14 日，P1）

- [ ] DHCP 失败时保留静态 IPv4 回退。
- [ ] 实现 `/api/status`、`/api/results` 和文件下载。
- [ ] 网络断开不影响本地检测。
- [ ] 记录堆、任务栈、固件大小和检测耗时。

### M6：稳定性、文档和演示冻结（目标 9 月 15–18 日）

- [ ] 连续 100 次检测或 1 小时运行无崩溃。
- [ ] 三次冷启动均能离线完成完整演示。
- [ ] 更新构建、烧录、操作、故障排查和架构文档。
- [ ] 归集并脱敏 AI Coding 日志。
- [ ] 完成 5 分钟内演示脚本和视频。

## 12. 当前下一步

M3 软件链路已经构建通过，下一步转入实板验收和阈值标定：

1. 烧录 M3 固件，依次验证“录入标准件 → READY → 检测 → PASS/FAIL → 停止”
   的状态、按钮和串口日志。
2. 录入固定工装标准件，确认三帧模板完成后正确件能够稳定 PASS，漏装和错位件
   能显示与实际位置一致的红色异常框。
3. 记录检测线程耗时和并发预览帧率；预览不得低于 8 fps，单次检测不得超过 1 s。
4. 以 18% 为起点标定面积阈值和 12% 块异常阈值，完成正确、漏装、错位三类
   各不少于 20 次统计；根据误判结果再调整固定 ROI 和阈值。

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
