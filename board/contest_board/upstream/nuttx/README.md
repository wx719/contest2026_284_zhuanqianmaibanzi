# NuttX 上游前置修复

比赛专属仓只保存 vendor 定制代码。以下补丁不是板级实现，而是当前 `dev-ai-contest-2026` NuttX 基线中的公共问题修复，需按参赛代码提交指南分别向公共 NuttX 仓发起 PR：

1. `0001-riscv-espressif-fix-kconfig-menu.patch`：修复公共 Espressif Kconfig 菜单边界错误，否则 Kconfig 在解析自定义 RISC-V 芯片配置时失败。
2. `0002-usrsock-guard-api-when-disabled.patch`：在未启用 USRSOCK 的最小 NSH 配置中保护其 API 声明，避免无网络构建引用未声明类型。
3. `0003-mmcsd-spi-verify-write-completion.patch`：写数据块后等待卡内部编程完成并读取 CMD13 状态，避免把编程超时或卡错误误报为写入成功。

补丁合入公共分支后，作品构建不再需要手工应用这些文件。不要把它们复制进 vendor 源文件或在构建规则中静默修改核心树。
