# TLS 证书指纹与 Xray 升级

## 审查结论

VLESS / Trojan / VMess 的链接导入此前仅按字符串是否非空判断 `allowInsecure`，因此遗漏 `insecure=1`，并错误地把 `allowInsecure=0` 当作开启。三个别名现在按 `allowInsecure`、`insecure`、`allow_insecure` 的优先级解析；仅 `1` / `true` 开启，不区分大小写。

`pcs` 保存证书 DER 的 SHA-256，接受 64 位十六进制或冒号分隔的 32 字节，以及逗号分隔的多个值。保存为无冒号的小写十六进制。Clash 的 `fingerprint` 同样解析；`chrome` 等已知浏览器名称继续忽略，其他非法值拒绝导入。新旧两种 VMess 分享格式均保留指纹与跳过 CA 校验的设置。

当前 wanqiua Xray 26.3.27-matsuri.1 使用原生 `pinnedPeerCertSha256`（十六进制，多个值用逗号分隔）。GUI 直接传递保存的证书指纹；原生指纹验证替代 CA 校验，不再强制开启 `allowInsecure`，服务器发送多张证书时仍可验证叶证书。TLS / Chrome uTLS 均拒绝错误指纹。

0.11 已发布包使用旧 `pinnedPeerCertificateChainSha256`（Base64 数组），只适用于服务器发送单张证书的情况。新内核仍保留该字段的整链语义，自定义配置不会自动把链指纹改成叶证书指纹；两种字段同时设置时都必须满足。

当前 sing-box 适配器不支持这些协议的证书 DER 指纹，自动模式选择 Xray。内核选择和配置生成入口都验证该限制，防止直接生成配置时静默丢失指纹。编辑器支持输入、规范化、保存、应用到分组和实时更新内核可用状态；非法指纹不能保存。未启用 TLS 时，已保存的非空指纹也会阻止生成配置。

REALITY 使用公钥认证，导入时忽略 `pcs`；已有配置或手动编辑同时设置 REALITY 与证书指纹时拒绝生成配置，提示清空指纹。Xray 支持 VLESS + REALITY，WebSocket + REALITY 仍选择 sing-box。

## 验证

- `go/cmd/nekoray_core/certificate_pin_test.go`：本地自签名 VLESS 服务器，TCP / WebSocket × Go TLS / Chrome uTLS × 单证书 / 多证书 × 正确 / 错误 / 多个指纹 / 未开启 allowInsecure，两种指纹字段共 64 个连接用例。
- `tests/core_capabilities_test.cpp`：解析、别名冲突、Clash 映射、两种分享格式往返、自动选择 Xray、直接配置生成拒绝不支持或非法指纹。
- `tests/gui_regression_test.cpp`：编辑器加载、规范化保存、取消草稿、实时更新内核选项，可用 `NEKORAY_TEST_SCREENSHOT_DIR` 输出界面截图。
- 本机使用项目指定 Qt 6.5.2 SDK、MSVC 14.51、yaml-cpp 0.7.0、ZXing 2.0.0、protobuf 21.4 编译。

本次接续验证使用本地模拟服务器，不把 Claude 之前的真实订阅节点实测当作本次实测。升级后应重新更新订阅，使原先遗漏的参数重新导入。只替换内核不足以获得新的链接解析和编辑界面，需要同时更新 GUI。没有指纹的节点不会因新增空字段而改变序列化身份；导入后配置实际改变的节点可能重新匹配并失去旧测速记录。
