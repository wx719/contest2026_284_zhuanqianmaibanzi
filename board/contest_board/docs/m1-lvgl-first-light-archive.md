# M1 LVGL First-Light 归档

- 归档日期：2026-08-29
- 里程碑：M1 LVGL 左右分屏 first-light
- 结论：功能验收通过，带一个不阻塞后续开发的显示已知限制

## 已完成范围

- 1024×600 RGB565 MIPI-DSI framebuffer 注册为 `/dev/fb0`。
- LVGL 静态左右分屏、设备状态卡、结果卡和右侧预览占位区域。
- GT911 注册为 `/dev/input0`，完成 180° 坐标转换并缩短轮询周期。
- Enroll Sample、Inspect、Stop 按钮均可触发状态和结果变化。
- 阈值支持滑块拖动及 `-`、`+` 单步调节，范围为 5%～40%。
- 启动时 framebuffer 清黑并同步缓存，未启动 `velapoka` 时不再出现底部残影。
- 状态文字限制宽度并右对齐，Enroll 后不再越出左侧面板。
- 阈值滑块已加粗并强化轨道、进度和手柄对比度。

## 实板验收结论

运行命令：

```text
nsh> velapoka
velapoka: LVGL first-light ready, fb=/dev/fb0 touch=/dev/input0
```

实板确认：

- 触摸 DOWN/UP 能稳定上报，按钮事件和串口日志一致。
- 三个按钮的状态文字与结果区域均能正常变化。
- 阈值滑块及 `-`、`+` 按钮可正常调整数值。
- 快速连续点击时屏幕仍可能出现撕裂，但不影响触摸事件、按钮逻辑或当前功能。

## 已知限制与技术决策

M1 使用单 framebuffer 和 `FBIO_UPDATE` 脏矩形缓存同步。CPU 在 LVGL direct
mode 下修改的也是 DSI/DW-GDMA 正在扫描的 framebuffer，因此高频刷新时无法从
架构上完全避免撕裂。将大脏矩形拆成 8 行缓存同步块可以降低长时间 PSRAM 争用，
但不能提供真正的无撕裂切屏。

该问题不继续阻塞 M1。M2 接入相机预览时统一恢复双 framebuffer，由 LVGL 在后备
buffer 渲染，并在 VSync 边界通过 pan display 切换；同时回归 CSI 与 DSI 共享
PSRAM/DW-GDMA 时的带宽和稳定性。

## 归档固件

- 文件：`nuttx/vela_nuttx.bin`
- 大小：688176 bytes
- SHA-256：`1fd3dd716b338858be24c4a9981851591d310724a8644462d77f55891b2e27ed`
- 构建配置：`vendor/openvela/boards/contest2026_284_board/configs/velapoka`
- 构建命令：`./build.sh vendor/openvela/boards/contest2026_284_board/configs/velapoka -j2`

该固件是工作区构建产物，不由团队仓直接跟踪。需要长期保存时，应在发布或制品
存储中以 SHA-256 校验值标识。

## M2 接续入口

1. 迁移 `camtest` 到团队仓，抽取 V4L2 采集逻辑。
2. 实现 RAW10 到 512×288 灰度预览的双缓冲发布。
3. 使用 LVGL Image 显示预览并叠加 ROI。
4. 恢复显示双 framebuffer 和 VSync 切换，消除快速点击撕裂。
5. 连续运行相机、触摸和显示 30 分钟，确认无 CSI 超时、DSI 黑屏或显存泄漏。
