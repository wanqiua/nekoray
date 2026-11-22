# NekoRay_BY_WanQiu 0.3

0.3 改进主界面连接面板，并为 Windows x64 便携版增加本仓库发行包更新流程。

## 主界面

- 移除顶部推广按钮。
- 日志 / 连接标签栏右侧新增收起和恢复按钮，恢复时回到收起前的分隔高度。
- 连接列表为空时说明原因：连接统计未启用、当前没有活动连接，或 sing-box 后端暂不支持枚举连接。Xray 连接列表仍需在基本设置中启用连接统计。

## 便携版更新

- 更新按钮读取 `wanqiua/nekoray` 最新稳定发行版，只选择 `NekoRay_BY_WanQiu-*-windows-x64.zip`。
- 下载完成后正常关闭主程序和内核，再由独立辅助程序校验 GitHub SHA-256（发行 API 提供时）、ZIP 路径并解压到暂存目录。
- 替换前将完整旧程序目录备份为同级 `NekoRay_BY_WanQiu.backup-YYYYMMDD-HHMMSS`。`config` 不会被更新包覆盖；安装失败时从备份恢复原文件。
- 更新成功后自动重新启动 `NekoRay_BY_WanQiu.exe`。如果文件夹不可写、下载中断或更新辅助程序缺失，更新会提示错误并保留现有安装。

## 下载与升级

发行包：`NekoRay_BY_WanQiu-0.3-windows-x64.zip`。解压完整目录后运行 `NekoRay_BY_WanQiu.exe`。支持 Windows 10/11 x64。

从早期便携版更新时，建议先将程序目录放在有写入权限的位置。自动更新会在旧目录旁保存完整备份，备份会占用约一个旧版程序目录的磁盘空间；需要回退时，退出程序后将备份目录内容复制回安装目录，并保留当前 `config`。

## 验证范围

Go 更新辅助程序单元测试覆盖配置保留、旧版备份和 ZIP 路径穿越拒绝。GUI 构建与便携包验收结果应记录在随发行包提供的 `NekoRay_BY_WanQiu-0.3-verification.md`。
