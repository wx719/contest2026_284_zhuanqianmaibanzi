# M4 MicroSD 留档与历史恢复归档

- 归档日期：2026-08-31
- 里程碑：M4 MicroSD 留档和历史记录
- 结论：核心功能、跨设备持久化、自动恢复与触屏安全退出通过实板验收

## 已完成范围

- ESP32-P4 SPI2 驱动 MicroSD，注册 `/dev/mmcsd0` 并自动挂载 FAT32。
- 应用启动时优先恢复历史、320×180 灰度模板、ROI 和检测阈值。
- 标准件模板以带版本、尺寸、ROI、payload 长度和 CRC32 的二进制格式保存。
- 检测结果按 JSONL 追加，包含编号、相机帧号、PASS/FAIL、得分、差异比例、
  耗时、阈值、图像相对路径和最多 8 个差异框。
- FAIL 检测保存 320×180、8-bit 灰度 BMP；PASS 记录不重复保存图像。
- 存储由优先级 90、深度 4 的后台队列处理，不阻塞 UI、相机和检测线程。
- 历史加载跳过重复或倒序 ID，下一编号从最大有效 ID 加 1。
- 产品配置上电后直接创建 VelaPoka 任务，同时保留 NSH 维护入口。
- 独立 `Exit` 按钮可停止相机和检测、排空存储队列、卸载 FAT 并返回 NSH。
- 长按 Stop 保留为安全退出备用操作。

完整采集、检测和 SD 写入的数据流见
[velapoka-capture-storage-chain.md](velapoka-capture-storage-chain.md)。

## 实板验收结论

### SPI2 与跨设备持久化

启动识别 16 GB MicroSD：

```text
MicroSD: /dev/mmcsd0 ready, sectors=31116288 sector_size=512
capacity=15931539456 bytes (SPI2, LDO4)
```

板端读取 Windows 写入的 `WINFIX.TXT` 成功；板端写入 `M4HW.TXT` 后执行
`umount /mnt/sdcard`、完全断电并将卡接入 Windows，Windows 能读取 `M4HW`。
这确认写入的是 SD 介质本身，而不是板端缓存或错误的目录视图。

### 应用文件落盘

应用完成录入和检测后确认存在：

```text
/mnt/sdcard/velapoka/
├── config.json                  108 B
├── results.jsonl
├── template.bin               57636 B
└── fail/
    ├── fail_00000001.bmp      58678 B
    ├── fail_00000002.bmp      58678 B
    └── ...
```

`template.bin` 的头部魔数为 `VKP1`，版本、头长、320×180 尺寸、ROI、阈值、
payload 长度和 CRC 字段均与运行配置一致。`results.jsonl` 中 FAIL 图像路径和差异
框坐标与串口检测结果一致。断电移卡后，模板、配置、JSONL 和 FAIL 目录均已在
Windows 文件系统中可见。

### 断电恢复与编号续接

已有记录包含一条旧的重复 ID。归档固件启动时输出：

```text
VelaPoka storage: history loaded=9 duplicates=1 skipped=0 next=10
VelaPoka storage: mounted /dev/mmcsd0 at /mnt/sdcard
```

应用随后自动恢复模板和阈值，从 `IDLE` 进入 `READY`。无需重新录入即可继续检测，
历史编号在多轮重启中先后续接为 `#3`～`#9`。归档验收轮重新录入后完成一次检测：

```text
VelaPoka state: READY -> CAPTURE
VelaPoka state: CAPTURE -> PREPROCESS
VelaPoka state: PREPROCESS -> INSPECT
VelaPoka state: INSPECT -> PASS
velapoka: result=PASS score=100.0 diff=0.0% boxes=0 time=10 ms sequence=3490
VelaPoka state: PASS -> SAVE_RESULT
VelaPoka state: SAVE_RESULT -> READY
velapoka: result queued as record #10
```

这证明最大有效 ID 恢复、运行期递增、模板录入、检测与后台留档能够形成闭环。

### 自动启动与触屏安全退出

归档固件上电进入 NSH 后无需输入 `velapoka`，产品任务自动启动。点击独立 Exit
按钮后串口顺序为：

```text
VelaPoka UI: action=4 click=4
velapoka: shutdown requested
SC2336 CSI: stopped, buffer requests=7129 finished frames=7128
SC2336: stream off, reg 0x0100=0x00
VelaPoka storage: unmounted /mnt/sdcard
velapoka: safe shutdown complete
nsh>
```

相机生产者先停止，Storage 线程随后排空待写请求并卸载由应用挂载的 FAT，最终
返回 NSH。该顺序通过实板验证，退出后可安全断电或拔卡。

## M4 期间修复的问题

### SC2336 父结构指针错位

`struct imgdata_s` 不是 `struct sc2336_dev_s` 的首成员，早期代码直接强转父结构，
导致 CSI 状态写入相邻内存并覆盖 MMC/SD 对象，最终触发 load access fault。修复为
按 `offsetof(struct sc2336_dev_s, data)` 还原父结构后，蓝屏消失。

### 已挂载 FAT 被重复挂载

手工预挂载 `/mnt/sdcard` 后，应用再次 mount 曾返回 `-ENOTDIR`。存储初始化改用
`statfs()` 判断目标是否已经是 FAT，并记录挂载所有权；只有应用自己完成的 mount
才在退出时 umount。

### FAT 长文件名未启用

`results.jsonl`、`config.json` 和 FAIL BMP 超出 FAT 8.3 名称限制，曾使历史加载
返回 `-EINVAL`。产品配置启用 `CONFIG_FAT_LFN` 并限制最大文件名为 32 B，同时
增加应用编译期配置守卫。

### 历史解析依赖未启用的 scanset

原 JSONL 解析使用 `%[...]`，但产品 libc 未启用 `CONFIG_LIBC_SCANSET`，导致记录
被静默跳过且编号回到 `#1`。解析改为标准定宽字符串和数字字段，并增加
`loaded/duplicates/skipped/next` 启动日志。

### GPIO 模拟 SPI 未真正持久化

GPIO 模拟 SPI 下，板端写入、读回和 umount 均成功，但完全断电移卡后 Windows
看不到新文件。改用 ESP32-P4 SPI2、GPIO Matrix 和软件 CS 后，`M4HW` 通过板端
卸载、断电和 Windows 读取，确认根因已消除。

### 产品固件未自动启动

ROMFS 启动脚本没有触发应用。产品配置改为在 `board_app_initialize()` 的 bring-up
成功后直接以优先级 100、16 KiB 栈创建 `velapoka_main`；NSH 仍保留用于维护和
故障恢复。

## 文件一致性与持久化语义

模板和配置使用 `temporary → fsync → close → rename`。检测记录进入队列时立即复制
结果，FAIL 时也复制归一化灰度图；串口的 `result queued` 只表示内存入队。看到
`VelaPoka storage: unmounted` 和 `safe shutdown complete` 后，才表示队列已排空且
FAT 已安全卸载。

## 归档固件

- 文件：`nuttx/vela_nuttx.bin`
- 大小：713792 bytes
- SHA-256：`08dc9eae1e52e2b0f4cb40fb6d79fdc6af3f1786f132531c4339539e5f694b60`
- ELF：`nuttx/nuttx`，1424768 bytes
- ELF SHA-256：`4bbe2f85fbed386eb90c8864e08ee04e897c22fce07436371f1b631aa55ba7f5`
- 构建配置：`vendor/openvela/boards/contest2026_284_board/configs/velapoka`
- 构建命令：`./build.sh vendor/openvela/boards/contest2026_284_board/configs/velapoka -j2`

该固件是工作区构建产物，不由团队仓直接跟踪。长期保存时应使用上述 SHA-256
标识对应制品。

## 保留回归项

以下项目不再阻塞 M4 归档，但必须在 M6 功能冻结前完成：

- 在 Windows 图片查看器中实际打开最新 FAIL BMP，确认灰度、方向和内容正确。
- 对照 `results.jsonl` 核对屏幕最近 3 条记录的编号顺序和 PASS/FAIL 类型。
- 连续 20 次检测期间确认预览不低于 8 fps、单次检测不超过 1 s，记录不丢失、
  不乱序。
- 三次完整断电冷启动均自动进入应用、恢复模板和阈值并连续续号。
- M6 的 100 次检测或 1 小时相机、显示、检测与存储并发压力测试。

## 后续入口

1. 返回 M3，完成正确件、漏装件、错位件各不少于 20 次的阈值标定和统计。
2. M3 退出条件满足后，根据剩余时间进入 M5 网络与工程增强。
3. 最后进入 M6，完成上述回归项、稳定性测试、文档、AI 日志和演示冻结。
