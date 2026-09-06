# 比赛交付合规自检

核对依据：openvela `dev-ai-contest-2026` 分支的《新平台适配指南》《最小可运行 NSH 系统 defconfig 参考》和《参赛代码提交指南》。

## 代码边界

- [x] 芯片层、板级层、链接脚本和 defconfig 全部位于本参赛仓 `board/contest_board/`。
- [x] manifest 将该目录映射到 `vendor/openvela/boards/contest2026_284_board`。
- [x] HAL 仓、目标文件和静态库均被忽略，不作为定制源码提交。
- [x] 构建规则不会静默修改 NuttX 核心代码。
- [x] `upstream/nuttx/` 中的三个公共修复已分别提交至 openvela/nuttx 的
  `dev-ai-contest-2026` 分支（PR #371、#372、#373）；在其合入前，本地复现需
  显式应用补丁。
- [ ] 按 `logs/README.md` 导出本次真实 AI Coding 会话；模板示例日志已删除，避免作为伪交付内容提交。

## L0 最小 NSH

- [x] RISC-V 架构、ESP32-P4 自定义芯片目录和 Function EV Board 自定义板目录已配置。
- [x] UART0 控制台为 115200 8N1，TX/RX 为 GPIO37/GPIO38。
- [x] `CONFIG_SYSTEM_NSH=y`，入口为 `nsh_main`。
- [x] idle 栈和中断栈均为 2048 字节，RR 时间片为 200 ms。
- [x] NET、PM、传感、音频、视频、LittleFS、TMPFS 和测试程序集均关闭；为让
  最小恢复镜像可检查 MicroSD，保留 SPI2、MMC/SD 和 FAT。
- [x] PROCFS/NSH_ARCHINIT 仅用于赛事建议的 `ps` 验证；dumpstack/backtrace 保留为移植期诊断。未增量启用外设子系统。

## 构建产物

2026-09-06 在 NuttX `5d51399cb05` 基线上重新配置并完成 `nsh` 全量构建：

| 产物 | 大小 | SHA-256 |
| --- | ---: | --- |
| `nuttx/staging/libarch.a` | 1423258 B | `0b38debf44a4d62d42232509815dff35af9471892948465ac6e6cdea29b2a8ce` |
| `nuttx/staging/libboards.a` | 2848 B | `3f07dbe77c9c1f64e741544cc036dd4ff540c1fc4c69799abec5cd1b1db8a204` |
| `nuttx/vela_nuttx.bin` | 252956 B | `6bfbe6517c75236e99942fc29bb2aeb6ed03fe60848324bae1ea8d5cb40960a2` |

## VelaPoka 基础外设配置

- [x] `configs/velapoka` 保留已验证的 UART0/NSH 恢复路径。
- [x] LCD reset、背光和 Ethernet PHY reset 以安全电平初始化；显示驱动就绪前背光保持关闭。
- [x] GPIO7/GPIO8 软件 I2C 与 GT911 touchscreen lower-half 完成编译和链接。
- [x] `/dev/velapoka` 区分“板载能力”与“本次启动已就绪能力”。
- [x] GT911 缺失或探测失败只记录错误，不阻断 NSH。
- [x] 7 英寸屏、GPIO/I2C 和 GT911 已完成实板 smoke test，显示与触摸分别注册为
  `/dev/fb0` 和 `/dev/input0`。
- [x] PSRAM 已识别并映射 32 MiB，且与 EMAC 同时启用时可正常启动。
- [x] EMAC 已注册为 NuttX `eth0`，RMII/SMI、PHY 链路、ARP 与双向 ICMP 实板验证通过。
- [x] SC2336/CSI 注册为 `/dev/video0`，连续采集完整帧和产品实时预览已通过。
- [x] MicroSD 通过 SPI2 注册为 `/dev/mmcsd0`；FAT32 挂载、读写、卸载、断电后
  Windows 读取，以及模板、JSONL 和 FAIL BMP 持久化均已通过。
- [x] Ethernet 与 MicroSD 初始化成功后分别设置 BSP ready 位；2026-09-06
  `nsh` 和 `velapoka` 重新构建通过，修正后的 ready 位仍待本次固件实板回归。

2026-09-06 的 `velapoka` 构建已完成编译、链接和 `esptool.py v4.8.1` 打包：

| 产物 | 大小 | SHA-256 |
| --- | ---: | --- |
| `nuttx/nuttx` | 1647188 B | `07fa14b647ffc6a7b127b6ea285ac30e1ddae88089eb010805bf4fa8ebd4ec7d` |
| `nuttx/vela_nuttx.bin` | 842260 B | `3170ab36266bd7ceef7055f2ea3752c9be75b3f2a95617d8ce3b902b6c9d712e` |

## 实板验证

目标硬件：ESP32-P4 revision v3.2，simple boot 镜像烧录至 `0x2000`。

- [x] 单次启动直接出现 `NuttShell (NSH)`。
- [x] `help`、`uname -a`、`ps`、`uptime` 执行成功。
- [x] UART TX/RX、任务调度和系统 tick 正常。
- [x] 三轮独立 USB-UART hard reset 均只出现 `CHIP_USB_UART_RESET`，随后直接进入 NSH；没有 LP/HP WDT 复位。

ROM 显示的 SHA-256 comparison warning 来自 Espressif `--ram-only-header` simple boot 镜像不附加 digest；ROM 随后正常加载并启动，不属于烧录校验失败。

## Ethernet 实板验收

2026-08-24 至 2026-09-06 使用产品固件完成验证：

| 项目 | 结果 |
| --- | --- |
| NuttX 网络设备 | `eth0` 注册成功，eFuse MAC `e8:f6:0a:e3:a9:35` |
| 链路状态 | `RUNNING`，MTU 1500 |
| 静态 IPv4 | 开发板 `10.0.0.2/24`，Windows 主机 `10.0.0.1/24` |
| 开发板 → 主机 | 10 发 10 收，0% 丢包 |
| 主机 → 开发板 | 4 发 4 收，0% 丢包 |
| PSRAM 共存 | 32 MiB PSRAM 初始化后 EMAC 正常注册和通信 |
| HTTP 导出 | 状态 JSON、完整 JSONL、FAIL BMP、方法与路径保护均通过 |
| 本地并发 | 网络请求期间样本录入、PASS/FAIL、SD 留档和断线恢复正常 |

当前结论覆盖静态 IPv4、ARP、ICMP、TCP HTTP 查询/导出和断线恢复。DHCP、
TCP/UDP 吞吐量及长时间压力仍未纳入已验证范围。
