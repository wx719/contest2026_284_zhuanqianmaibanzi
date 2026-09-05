# VelaPoka ESP32-C6 Wi-Fi 驱动适配

## 范围

本次只增加板级 Wi-Fi 驱动，不增加 Wi-Fi 业务应用、开机配网、DHCP 策略或
云服务。`configs/velapoka` 启用驱动，最小恢复配置 `configs/nsh` 保持不变。

板载 ESP32-C6 运行 ESP-Hosted slave，ESP32-P4 运行 openvela host。硬件链路
遵循 Espressif 的 ESP32-P4 Function EV Board 参考配置：CLK GPIO18、CMD
GPIO19、D0～D3 GPIO14～17、C6 reset GPIO54。协议和开发板配置可对照
[ESP-Hosted SDIO 文档](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/sdio.md)
及 [ESP32-P4 Function EV Board 配置](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/esp32_p4_function_ev_board.md)。

## 驱动链路

```text
ESP32-C6 ESP-Hosted slave
          ⇅ SDIO 4-bit / GPIO54 reset
ESP32-P4 SDMMC host
          ⇅ CMD52/CMD53 + SDIO interrupt
ESP-Hosted transport / RPC
          ⇅ STA control + Ethernet frames
NuttX wlan0 netdev / wireless ioctl
```

- ESP-Hosted transport 统一通过 GPIO54 复位 C6；`velapoka_hosted_sdio.c`
  完成 SDIO 枚举、function 1 使能、4-bit 切换、20 MHz 时钟及
  CMD52/CMD53 传输，不再二次复位 C6。CMD53 使用内部 DRAM、缓存行对齐的
  bounce buffer，允许 ESP-Hosted 调用方的栈或数据缓冲区位于 PSRAM。传输层
  packet mempool 使用普通 `malloc/free` 配对，不占用紧张的 kernel/DMA heap。
- ESP-Hosted host 固定使用组件版本 0.0.6 对应的官方提交
  `65ebfacb7608889fe854e3442817e1998e4a4d9e`，并以 SHA-256 校验归档；版本
  信息见 [Espressif Component Registry](https://components.espressif.com/components/espressif/esp_hosted/versions/0.0.6?language=en)。
- `velapoka_esp_hosted.c` 注册 `wlan0`，在 LPWORK 中衔接 NuttX IOB 与
  ESP-Hosted STA 数据通道。`VELAPOKA_WIFI` 同时选择
  `DRIVERS_IEEE80211`，确保 `netdev_register()` 包含
  `NET_LL_IEEE80211`/`wlan%d` 注册分支。
- wireless ioctl 覆盖 STA ESSID、PSK、BSSID、WPA/WPA2/WPA3、扫描、模式和
  RSSI。AP、Bluetooth 和业务层自动联网不在本次范围内。
- `VELAPOKA_CAP_WIFI` 仅在 SDIO transport、RPC、STA 启动和 `wlan0` 注册全部
  成功后置位；C6 不在线时握手在有限超时后失败，不阻塞其余板级外设启动。

## 构建

在 openvela 工作区根目录执行：

```bash
./build.sh vendor/openvela/boards/contest2026_284_board/configs/velapoka -j2
```

成功产物为 `nuttx/vela_nuttx.bin`。构建会获取并校验 ESP-Hosted 0.0.6，然后
应用 `chips/esp32p4/patches/0006-esp-hosted-openvela-port.patch`。

## 官方初始化顺序

驱动按 ESP-Hosted 和 ESP-IDF 的 SDIO 初始化路径执行：

1. transport 将 GPIO54 `C6_CHIP_PU` 按高、低、高的顺序翻转，低电平复位
   C6，最后回到高电平释放并启动 C6；
2. P4 创建 SDMMC host/slot，初始阶段保持 SDIO init 时序；
3. CMD52 写 CCCR `IO_ABORT` 的 reset 位；该命令因从机立即复位而超时也可
   继续；
4. CMD0 后用 CMD5 探测 OCR，再轮询 ready 位，并使能 P4 host 的 SDIO IO
   中断；
5. CMD3/CMD7 分配 RCA 并选卡；
6. CMD52 使能 function 1、中断和 512-byte block size；
7. 卡端先切换 4-bit，随后将 P4 slot 配置为 4-bit/20 MHz；
8. ESP-Hosted 协议寄存器地址先使用 `ESP_ADDRESS_MASK` 转为 function 1
   地址，再通知 slave 打开数据通道并等待 `INIT` event。

启动时会打印第一次 CMD5 返回的 OCR 和 function 数。如果仍然失败，末条
日志会同时给出错误码、最后一次 OCR 和重试次数，可区分“命令无响应”和
“从机有响应但 ready 位未置位”。

## 实板验收

2026-09-05 使用产品镜像完成驱动层实板验收：

1. CMD5 返回 `OCR=20ffff00 functions=2`，function 1 在 4-bit、20 MHz 下就绪；
2. P4 收到 ESP32-C6 INIT event，ESP-Hosted transport 建立完成；
3. `WifiInit`、`SetWifiMode`、`WifiStart` 和 `GetMACAddress` RPC 均收到响应；
4. `wlan0` 注册成功，MAC 为 `10:bd:a3:8a:bc:55`，`ifconfig` 显示接口 UP；
5. `/dev/velapoka` 返回 `ready=0x0000033f`，其中包含 Wi-Fi 位 `0x200`；
6. 启动过程没有 SDIO、mempool 或 RPC 超时错误。

验收镜像 `vela_nuttx.bin` 大小为 837544 字节，SHA-256 为
`13603430ecf4422aad953152d360f4af87b63fffc992f2cfc461a451bad05a80`。

当前产品配置没有加入 WAPI、配网或自动联网应用，因此 `wlan0` 地址仍为
`0.0.0.0`。热点扫描、AP 关联、DHCP、Ping 和长时间数据收发属于后续测试镜像
范围，不影响本次“仅驱动调通”的验收结论。

CMD5 的 function 数为 2 是 ESP-Hosted SDIO 协议的正常值，host 数据通道使用
function 1。若步骤 1 失败，优先检查 C6 slave 固件是否为与 host 组件匹配的
ESP-Hosted 版本、GPIO54 复位电平，以及 GPIO14～19 是否被其他外设占用。若能
枚举但大帧收发失败，重点检查 CMD53 块传输、余数字节传输和 SDIO 中断计数。
