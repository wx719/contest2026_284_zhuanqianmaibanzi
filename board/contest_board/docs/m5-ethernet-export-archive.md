# M5 RJ45 数据查询与导出归档

- 归档日期：2026-09-06
- 里程碑：M5 网络导出与工程增强
- 结论：RJ45 核心查询、记录导出、异常图片下载、访问限制、断线恢复和安全退出
  已通过实板验收；工程指标统计仍保留为 M5 未完成项

## 已完成范围

- 产品自动为 `eth0` 配置静态地址 `10.0.0.2/24`，监听 TCP 8080。
- 提供只读 HTTP 服务：状态 JSON、完整结果 JSONL 和指定 FAIL BMP 下载。
- 导出路径使用严格白名单，不允许读取配置、模板或目录外文件。
- SD 文件先在存储锁内生成不超过 4 MiB 的内存快照，再释放锁向客户端发送，
  网络慢客户端不会持续占用写盘锁。
- HTTP 监听启用 TCP backlog，并对客户端收发设置超时，避免触屏 Exit 被异常连接
  无限阻塞。
- 网络失败不参与本地相机、检测、界面和存储链路的成功条件。
- 点击触屏 `Exit` 时先停止 HTTP 服务，再停止相机、排空存储队列并卸载 FAT。
- 物理复位后产品和 HTTP 服务自动启动，不需要通过 NSH 输入网络命令。

## 接口边界

| 方法和路径 | 返回内容 | 实板结果 |
| --- | --- | --- |
| `GET /` | 服务名称和接口索引 | HTTP 200 |
| `GET /api/status` | 状态、设备在线状态、阈值和最新记录号 | HTTP 200 |
| `GET /api/results` | MicroSD 上完整的 `results.jsonl` | HTTP 200，2775 B |
| `GET /files/fail/fail_XXXXXXXX.bmp` | 指定 FAIL 灰度图片 | HTTP 200，样例 58678 B |
| 非 `GET` 方法 | JSON 错误 | HTTP 405 |
| 非白名单或不存在的文件 | JSON 错误 | HTTP 404 |

当前服务是面向比赛演示和局域网导出的只读 API，不包含上传、删除、远程控制或
图形化 HTML 管理页。

## 实板验收证据

### 链路与自动启动

Windows 有线网卡设置为 `10.0.0.1/24` 后，对 `10.0.0.2` 连续 Ping 4 次均收到
回复，丢包率为 0%，平均往返时间约 5～11 ms。应用串口自动输出：

```text
VelaPoka network: http://10.0.0.2:8080 ready on eth0
```

不需要在 NSH 中手动执行 `ifconfig`、路由或 HTTP 启动命令。

### 状态和结果导出

`GET /api/status` 返回实板运行状态：

```json
{"service":"VelaPoka","state":"READY","interface":"eth0","ip":"10.0.0.2","port":8080,"camera_online":true,"storage_online":true,"reference_ready":true,"threshold_percent":18,"latest_record_id":12,"history_count":3}
```

`GET /api/results` 下载得到 2775 B 的完整 JSONL，包含历史 `#1`～`#12`，其中
PASS 记录不带图像，FAIL 记录包含相对 BMP 路径及差异框。旧介质上已有一条重复
`id=1`，应用启动日志将其统计为 `duplicates=1`，后续编号仍从最大有效 ID 续接。

### FAIL BMP 下载

按记录 `#11` 的路径下载：

```text
/files/fail/fail_00000011.bmp
```

Windows 得到 `fail_00000011.bmp`，文件大小为 58678 B，并已调用系统图片查看器
打开。该大小与 320×180、8-bit 灰度 BMP 的板端文件一致。

### 方法和路径保护

以下实板请求符合预期：

```text
POST /api/status                    -> 405 Method Not Allowed
GET  /files/config.json             -> 404 Not Found
GET  /files/../config.json          -> 404 Not Found
```

配置、模板及任意目录外文件不能通过 HTTP 导出。

### 网络负载、断线和恢复

Windows 连续请求 `/api/results` 期间，触屏仍完成模板录入、一次 FAIL 和一次 PASS，
本地检测耗时均为 10 ms，记录继续写入为 `#13`、`#14`。点击 Exit 后 HTTP 服务按
设计停止，剩余客户端连接失败，但设备仍可 Ping；相机停止、SD 卸载并出现
`velapoka: safe shutdown complete`。

重新启动并恢复链路后，最初一次请求在 PHY/链路协商期间超时，随后两次
`/api/status` 均成功，返回 `latest_record_id=17`，且相机、存储和模板状态均为
在线。这证明网络恢复后服务自动可用，本地记录在网络不可用期间仍可继续增长。

## 使用与生命周期约束

- 上电或复位后等待串口出现网络 ready 日志；没有串口时，至少等待 10～15 秒并
  确认 Ping 稳定后再打开接口。
- 拔插网线后首个 HTTP 请求可能因 PHY 重新协商而超时，重试即可；这不是应用
  数据丢失。
- 点击 `Exit` 表示安全停止整个产品应用，HTTP 8080 会主动关闭；此时仍能 Ping
  `eth0` 并不表示 HTTP 应继续服务。
- 需要再次演示时复位开发板，产品、相机、存储和 HTTP 会自动启动。

## 归档固件

- 文件：`nuttx/vela_nuttx.bin`
- 大小：842244 bytes
- SHA-256：`07321e95d3f9d663c79b4ac8eef8d4b92363d016c30871fd7a7e9d347c1455ef`
- 构建配置：`vendor/openvela/boards/contest2026_284_board/configs/velapoka`
- 构建命令：`./build.sh vendor/openvela/boards/contest2026_284_board/configs/velapoka -j2`

该候选在干净依赖恢复后完成全量产品构建；配置确认启用 `CONFIG_NET_SOCKOPTS`、
`CONFIG_NET_TCPBACKLOG` 和 8 个 backlog connection。源码通过 `git diff --check`，
新增网络代码通过 NuttX `checkpatch.sh` 检查。

## 保留项

以下项目不影响比赛描述中“通过 RJ45 提供记录查询和导出”的核心承诺，但仍须在
功能冻结前处理或明确取舍：

- M5 工程指标：记录 heap、各关键任务栈余量、固件大小和检测耗时统计。
- 当前输出为原始 JSON/JSONL 和 BMP；若时间允许，可增加只读图形化网页和一个
  “导出”触屏入口，但不应破坏已验证 API。
- M6 执行连续 100 次检测或 1 小时运行、三次冷启动、最近 3 条历史一致性和
  网络并发压力回归。
- M3 仍需固定工装和照明下完成正确件、漏装件、错位件各不少于 20 次的标定统计。

## 后续入口

1. 返回 M3，完成三类样本标定并固定阈值。
2. 补齐 M5 工程指标；图形化网页按比赛演示时间决定是否加入。
3. 进入 M6，完成稳定性、演示脚本、文档和日志冻结。
