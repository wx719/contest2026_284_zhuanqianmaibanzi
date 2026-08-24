# 比赛交付合规自检

核对依据：openvela `dev-ai-contest-2026` 分支的《新平台适配指南》《最小可运行 NSH 系统 defconfig 参考》和《参赛代码提交指南》。

## 代码边界

- [x] 芯片层、板级层、链接脚本和 defconfig 全部位于本参赛仓 `board/contest_board/`。
- [x] manifest 将该目录映射到 `vendor/openvela/boards/contest2026_284_board`。
- [x] HAL 仓、目标文件和静态库均被忽略，不作为定制源码提交。
- [x] 构建规则不会静默修改 NuttX 核心代码。
- [ ] `upstream/nuttx/` 中的两个公共修复需按规则分别提交至 openvela/nuttx 的 `dev-ai-contest-2026` 分支；在其合入前，本地复现需显式应用补丁。
- [ ] 按 `logs/README.md` 导出本次真实 AI Coding 会话；模板示例日志已删除，避免作为伪交付内容提交。

## L0 最小 NSH

- [x] RISC-V 架构、ESP32-P4 自定义芯片目录和 Function EV Board 自定义板目录已配置。
- [x] UART0 控制台为 115200 8N1，TX/RX 为 GPIO37/GPIO38。
- [x] `CONFIG_SYSTEM_NSH=y`，入口为 `nsh_main`。
- [x] idle 栈和中断栈均为 2048 字节，RR 时间片为 200 ms。
- [x] NET、PM、传感、音频、视频、FAT、LittleFS、TMPFS 和测试程序集均关闭。
- [x] PROCFS/NSH_ARCHINIT 仅用于赛事建议的 `ps` 验证；dumpstack/backtrace 保留为移植期诊断。未增量启用外设子系统。

## 构建产物

2026-08-09 在干净 NuttX `5d51399cb05` 基线上完成 `distclean -> configure -> olddefconfig -> make -j2`：

| 产物 | 大小 | SHA-256 |
| --- | ---: | --- |
| `nuttx/staging/libarch.a` | 1.4 MiB | `cbfb105c4481eb0471b6098a9d19fa769d09b84b4abae68c0b63220b543fd49c` |
| `nuttx/staging/libboards.a` | 3.0 KiB | `66eea2dd29a6d3aed2592eaff65a1d0411b02ec140da9c5aad7c30fcaddc9ea6` |
| `nuttx/vela_nuttx.bin` | 222 KiB | `8ff329a8e62b975a70164b3b2f2c93dd198fcb8118b6cfe3bfafc8779a8cb8a8` |

## VelaPoka 基础外设配置

- [x] `configs/velapoka` 保留已验证的 UART0/NSH 恢复路径。
- [x] LCD reset、背光和 Ethernet PHY reset 以安全电平初始化；显示驱动就绪前背光保持关闭。
- [x] GPIO7/GPIO8 软件 I2C 与 GT911 touchscreen lower-half 完成编译和链接。
- [x] `/dev/velapoka` 区分“板载能力”与“本次启动已就绪能力”。
- [x] GT911 缺失或探测失败只记录错误，不阻断 NSH。
- [ ] VelaPoka 配置仍需连接 7 英寸屏后完成 GPIO/I2C/GT911 实板 smoke test。
- [x] PSRAM 已识别并映射 32 MiB，且与 EMAC 同时启用时可正常启动。
- [x] EMAC 已注册为 NuttX `eth0`，RMII/SMI、PHY 链路、ARP 与双向 ICMP 实板验证通过。
- [ ] MIPI-CSI 和 SDMMC 仍未完成对应设备节点与实板 smoke test。

2026-08-10 的 `velapoka` 构建已完成编译、链接和 `esptool.py v4.8.1` 打包：

| 产物 | 大小 | SHA-256 |
| --- | ---: | --- |
| `nuttx/nuttx` | 424120 B | `2893b9ed5786e3afe07701d82fac71f3fcd9860419cbb356e68f38c8f4d7dcc5` |
| `nuttx/vela_nuttx.bin` | 245356 B | `3ddf906e8f2217648149b3da9e3e8d06c366a994ba4bc1751db2f73ee3ac6499` |

详细记录见 `validation/2026-08-10-velapoka-basic-build.txt`。

## 实板验证

目标硬件：ESP32-P4 revision v3.2，simple boot 镜像烧录至 `0x2000`。

- [x] 单次启动直接出现 `NuttShell (NSH)`。
- [x] `help`、`uname -a`、`ps`、`uptime` 执行成功。
- [x] UART TX/RX、任务调度和系统 tick 正常。
- [x] 三轮独立 USB-UART hard reset 均只出现 `CHIP_USB_UART_RESET`，随后直接进入 NSH；没有 LP/HP WDT 复位。

ROM 显示的 SHA-256 comparison warning 来自 Espressif `--ram-only-header` simple boot 镜像不附加 digest；ROM 随后正常加载并启动，不属于烧录校验失败。

## Ethernet 实板验收

2026-08-24 使用清理移植期诊断日志后的 `velapoka` 固件完成验证：

| 项目 | 结果 |
| --- | --- |
| NuttX 网络设备 | `eth0` 注册成功，eFuse MAC `e8:f6:0a:e3:a9:35` |
| 链路状态 | `RUNNING`，MTU 1500 |
| 静态 IPv4 | 开发板 `10.0.0.2/24`，Windows 主机 `10.0.0.1/24` |
| 开发板 → 主机 | 10 发 10 收，0% 丢包 |
| 主机 → 开发板 | 4 发 4 收，0% 丢包 |
| PSRAM 共存 | 32 MiB PSRAM 初始化后 EMAC 正常注册和通信 |
| 最终固件 | `vela_nuttx.bin`, SHA-256 `f4367e9dd670017cae3fc3f283b94dbe0157feddf2f5822742b7f1362b600c77` |

本次结论覆盖静态 IPv4、ARP 和 ICMP 双向通信。DHCP、TCP/UDP 吞吐量、长时间压力以及应用层 HTTP REST 数据导出尚未纳入已验证范围。
