# 版本记录

从 **NekoRay_BY_WanQiu 0.1** 开始使用独立版本号，Git 标签为 `v0.1`。后续版本使用新的递增版本号与标签；上游基线 3.26 仅保留在来源说明及构建清单的 `gui_base` 字段中。

| 版本 | 主要内容 |
| --- | --- |
| [v0.2](https://github.com/wanqiua/nekoray/releases/tag/v0.2) | 升级 wanqiua Xray 26.3.27-matsuri.1；HTTPUpgrade、原生证书指纹、旧配置迁移和 Windows UDP 双栈网卡绑定适配。 |
| [v0.11](https://github.com/wanqiua/nekoray/releases/tag/v0.11) | Hysteria2 自签名证书按指纹验证；insecure 别名解析；VLESS / Trojan / VMess 证书指纹导入、编辑、分享与 Xray 校验。 |
| [v0.1](https://github.com/wanqiua/nekoray/releases/tag/v0.1) | 新产品名称；删除不可用配置反馈及有效结果判断；VLESS XUDP 的 Xray 映射；Hysteria2 证书指纹与订阅端口别名；正常阻断日志及主界面高度修复。 |
| [v3.26-dual-core.2](https://github.com/wanqiua/nekoray/releases/tag/v3.26-dual-core.2) | 历史版本：双内核切换、TUN 恢复、批量 URL 测试，以及 AnyTLS、Snell 和 Hysteria2 扩展。 |
| [v3.26-dual-core.1](https://github.com/wanqiua/nekoray/tree/v3.26-dual-core.1) | 补录的历史开发基线，未包含后续 TUN 与新协议改动。 |

历史标签和发行文件保持原样。当前发行包名称为 `NekoRay_BY_WanQiu-0.2-windows-x64`，包含便携 ZIP、对应源码 ZIP、SHA-256 文件和验证记录。主程序为 `NekoRay_BY_WanQiu.exe`，窗口及 Windows 文件属性使用新名称与版本；原配置目录和两个内核文件名继续兼容。

0.2 将 Xray 从 Matsuri **1.8.4** 升级到 **wanqiua 26.3.27-matsuri.1**，sing-box 仍为 **1.14.0**。内核来源见 [构建说明](Build_Core.md)，本版变化见 [0.2 说明](Release_0.2.md)。

发布步骤：

1. 完成代码和检查，更新产品版本及发行说明，提交源码。
2. 构建与验证便携包、对应源码、依赖声明和校验文件。
3. 在已验证提交上创建新的带注释标签，将版本分支和新标签推送到 `origin`。
4. 创建对应 GitHub Release，上传产物并核对远端哈希，最后公开发布。

普通分支推送不会自动创建版本标签或发行文件。
