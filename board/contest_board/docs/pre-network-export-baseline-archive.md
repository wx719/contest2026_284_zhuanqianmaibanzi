# RJ45 数据导出开发前阶段基线归档

- 归档日期：2026-09-06
- 代码基线：`dc7edf9` (`feat(board): add ESP-Hosted Wi-Fi support`)
- 目的：在增加 RJ45 记录查询与导出前，冻结已通过实板验证的离线产品基线

## 已完成并保留的基线

- ESP32-P4 上电后能进入 NuttX，UART/NSH 维护通道保留。
- 32 MiB PSRAM、1024×600 RGB565 双 framebuffer 和 MIPI-DSI 正常。
- GT911 在 `/dev/input0` 注册，产品界面可全程触摸操作。
- SC2336 在 `/dev/video0` 注册，512×288 灰度预览实板稳定达到 15 fps，
  双缓冲和 VSync 切换无可见撕裂。
- 三帧平均标准件模板、固定 ROI、亮度归一化、块差/边缘差、PASS/FAIL、
  得分和异常框已进入产品应用。
- MicroSD 通过 ESP32-P4 SPI2 注册为 `/dev/mmcsd0`；模板、配置、JSONL 和
  FAIL BMP 经卸载、断电和 Windows 读取确认持久化。
- 历史记录能跨重启恢复并从最大有效 ID 续号；实板已从 `next=10` 生成
  `#10` PASS 记录。
- 产品任务可自动启动；触屏 Exit 按顺序停止相机、排空存储队列、
  卸载 FAT 并返回 NSH。
- RJ45 底层已注册 `eth0`，静态 IPv4、ARP 和与 Windows 主机双向 Ping 通过。
- ESP32-C6 通过 SDIO 4-bit/20 MHz 完成 ESP-Hosted 枚举、RPC、STA 启动、
  MAC 获取与 `wlan0` 注册。

M2 预览和 M4 存储的详细实板证据分别见
[m2-camera-preview-archive.md](m2-camera-preview-archive.md) 和
[m4-storage-archive.md](m4-storage-archive.md)。

## 本次归档不声称完成的内容

- M3 算法功能已完成，但正确件、漏装件、错位件各不少于 20 次的标定和
  统计尚未完成，因此 M3 退出条件仍未达到。
- RJ45 目前只验证网卡和 IP 基础链路，还没有实现比赛项目描述中承诺的
  检测状态查询、历史记录导出和 FAIL 图片下载。
- Wi-Fi 仅归档驱动层能力，没有将扫描、关联、DHCP 或产品数据传输列为
  本阶段验收结论。
- 连续 100 次检测或 1 小时并发压力、三次冷启动、Windows 打开最新
  FAIL BMP、最近 3 条历史与 JSONL 一致性属于 M6，尚未完成。

## RJ45 导出开发边界

后续网络功能必须保持本归档的离线闭环：断网或客户端访问失败不得阻塞
相机、检测、LVGL 和存储线程。最小交付范围为：

1. 查询设备、检测和 SD 卡状态。
2. 只读导出 `results.jsonl`。
3. 按记录中的相对路径下载 FAIL BMP。
4. 使用现有静态 IPv4 完成 Windows 客户端实板验收；DHCP 可在基本导出
   通过后增强。

## 后续顺序

1. 先固定工装和照明，完成 M3 三类各 20 次标定与统计。
2. 在 `eth0` 现有基础上实现只读 HTTP 查询和文件导出。
3. 进入 M6，完成断网回归、压力测试、冷启动、文档和演示冻结。
