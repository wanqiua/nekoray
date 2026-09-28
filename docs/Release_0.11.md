# NekoRay_BY_WanQiu 0.11

本版修复订阅节点的 TLS 参数解析和证书指纹验证，保留 Matsuri Xray 1.8.4 与 sing-box 1.14.0 双内核。

## 修复与新增

- Hysteria2 配置证书指纹后按指纹验证，自签名证书无需勾选“不检查服务器证书”；错误指纹仍拒绝连接。同时修复指纹与“不发送 SNI”组合的连接问题。
- VLESS / Trojan / VMess 支持 `allowInsecure`、`insecure`、`allow_insecure` 别名，正确区分 `1/true` 与 `0/false`，冲突时按上述顺序取值。
- 支持链接 `pcs` 和 Clash 十六进制 `fingerprint`，保留到编辑、保存和分享，包括旧版 VMess JSON 分享格式。
- TLS 设置增加“证书 SHA-256”输入框，支持冒号格式及逗号分隔多个指纹，保存时规范化。
- 配置指纹的上述协议在自动模式选择 Xray；sing-box 明确提示不支持，配置生成入口也会拒绝，避免静默忽略指纹。

## 兼容边界

当前 Xray 1.8.4 校验整条证书链的 SHA-256，此适配适用于服务器仅发送单张证书的情况。服务器发送多张证书时，单证书指纹会校验失败；不会放行错误指纹。

Xray 支持 VLESS + REALITY，但本版固定的 Xray 不支持 WebSocket + REALITY，这种组合继续选择 sing-box。REALITY 使用公钥认证，不使用证书 `pcs`。

## 升级

下载 `NekoRay_BY_WanQiu-0.11-windows-x64.zip`，完整解压。退出旧程序并备份 `config`，将该目录复制到新程序目录，再启动并更新订阅。更新后重新测试节点；配置实际发生变化的节点可能不再保留之前的测速记录。

本版需要新版 GUI 和随包内核配套使用。不要只覆盖一个内核，也不要把配置中的证书指纹替换成公钥指纹。

## 验证范围

本地自签名 VLESS 服务器覆盖 TCP / WebSocket、普通 TLS / Chrome uTLS、单证书 / 多证书、正确 / 错误 / 多个指纹以及 CA 校验，共 32 个连接用例。另有链接 / Clash 导入、分享往返、内核选择、配置生成和编辑器回归。

详细结果与产物哈希见发行附件 `NekoRay_BY_WanQiu-0.11-verification.md`。本次接续验证没有重新连接真实订阅服务器。实现与限制见 [TLS 证书指纹修复说明](TLS_Certificate_Pins.md)。
