# M5 RJ45 数据查询与导出实板验收

- 验收日期：2026-09-06
- 接口：板载 RJ45，对应 NuttX `eth0`
- 设备地址：`10.0.0.2/24`
- HTTP 地址：`http://10.0.0.2:8080`
- 当前结论：以下核心项目已通过实板验收；证据和保留项见
  [m5-ethernet-export-archive.md](m5-ethernet-export-archive.md)

## 功能边界

服务只提供查询和下载，不接受上传、删除或修改：

| 路径 | 内容 | Content-Type |
| --- | --- | --- |
| `/` | 服务名称和接口索引 | `application/json` |
| `/api/status` | 状态机、相机、SD、模板、阈值和最近记录号 | `application/json` |
| `/api/results` | MicroSD 上完整的 `results.jsonl` | `application/x-ndjson` |
| `/files/fail/fail_XXXXXXXX.bmp` | 指定 FAIL 灰度图片 | `image/bmp` |

文件导出采用严格路径白名单，不提供目录遍历。读取时只在 SD I/O 锁内生成不超过
4 MiB 的内存快照，随后释放锁再发送；因此慢速或断开的客户端不会长时间占用
存储写盘锁。本地相机、检测、界面和 SD 留档不依赖网络成功。

## 接线和 Windows 配置

1. 将开发板 RJ45 与电脑网口直连或接入同一交换机。
2. 在 Windows 的以太网 IPv4 属性中使用手动地址：
   - IP 地址：`10.0.0.1`
   - 子网掩码：`255.255.255.0`
   - 默认网关和 DNS：留空
3. 烧录并启动产品固件。串口应出现：

```text
VelaPoka network: http://10.0.0.2:8080 ready on eth0
```

没有该日志时先记录串口中的 `VelaPoka network:` 错误，不要用 NSH 手工配置 IP
掩盖自动启动问题。

## 基本接口测试

在 Windows PowerShell 或 CMD 执行：

```powershell
ping 10.0.0.2
curl.exe http://10.0.0.2:8080/
curl.exe http://10.0.0.2:8080/api/status
curl.exe http://10.0.0.2:8080/api/results -o results.jsonl
type results.jsonl
```

验收要求：

- Ping 有回复，三个 HTTP 请求均返回成功。
- `/api/status` 中 `interface` 为 `eth0`、`ip` 为 `10.0.0.2`，应用运行时
  `camera_online` 和 `storage_online` 为 `true`。
- 下载的 `results.jsonl` 与拔卡后 Windows 直接读取的文件内容一致。

## FAIL BMP 下载测试

先在 `results.jsonl` 中找一条 `result` 为 `FAIL` 且 `image` 非空的记录。例如：

```json
{"id":9,"result":"FAIL","image":"fail/fail_00000009.bmp"}
```

把 `image` 前加 `/files/` 后下载：

```powershell
curl.exe http://10.0.0.2:8080/files/fail/fail_00000009.bmp -o fail_00000009.bmp
dir fail_00000009.bmp
```

用 Windows 图片查看器打开 BMP。验收要求为文件非空、能够打开、画面与该次异常
记录对应。不存在的合法编号应返回 HTTP 404；任意其他文件路径也不能被导出。

## 断网与安全退出回归

1. 保持应用运行，拔掉网线。
2. 通过触屏重新录入模板，完成至少一次 PASS 和一次 FAIL 检测。
3. 确认预览、按钮、判定、红框和记录编号继续工作，串口无崩溃或存储错误。
4. 插回网线，等待 10～15 秒或直至 Ping 稳定，再次访问 `/api/status` 和
   `/api/results`，确认新记录可见。PHY 重新协商期间首个请求超时可直接重试。
5. 点击触屏 `Exit`，确认返回 NSH，并出现存储卸载和
   `velapoka: safe shutdown complete`。

2026-09-06 实板已完成以上核心验收：状态、完整 JSONL、FAIL BMP、405/404 路径
保护、网络请求期间本地检测、重连恢复以及触屏安全退出均符合预期。M5 的前三项
已完成；heap、关键任务栈、固件大小和检测耗时的集中工程指标记录仍待补齐。
