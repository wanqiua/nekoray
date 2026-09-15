# 阶段 4 服务端互通验证路线

2026-09-13 基于项目锁定的 sing-box **1.14.0** 源码、随该版本发布的文档及现有测试核对。此文件是可执行路线与依赖说明；本轮未实现 AnyTLS、Snell 新协议界面，也未把解析通过写成互通通过。

## 可直接利用的基础

三种协议在当前版本均有原生服务端，可在 Windows 回环环境中先完成真实连接、负载传输、错误凭据、取消和重连验证，不必先部署公网服务器。

`go/cmd/nekobox_core/protocol_test.go` 已有 `loopbackCertificate()`（ECDSA P-256 自签证书）和 `TestCurrentProxyProtocols`（真实回环代理到 HTTP 目标）。阶段 4 可复用这些基础，构造两个独立 `NewBoxInstance`，分别负责服务端和客户端，再增加 UDP 回显与并发负载验证。服务端只监听 `127.0.0.1` 随机端口；证书直接传给测试客户端，不写系统证书库。

| 协议 | 客户端配置 | 原生服务端配置 | 当前依赖与运行条件 |
|---|---|---|---|
| AnyTLS | `type=anytls`，password、启用 TLS | `type=anytls`，users 中相同 password、TLS 证书和私钥 | `github.com/anytls/sing-anytls v0.0.11`；TCP；无额外构建标签 |
| Snell 4 | `type=snell`，**version=4**、psk | `type=snell`，**version=5**、相同 psk | `github.com/sagernet/sing-snell v0.0.0-20260829071736-20f2eaec77c3`；TCP；无额外标签 |
| Snell 6 | `type=snell`，version=6、psk、mode | `type=snell`，version=6、相同 psk、mode | 相同 sing-snell；PSK 长度为 12–255 字节 |
| Hysteria2 | `type=hysteria2`，password、TLS | `type=hysteria2`，users、TLS 证书和私钥 | `github.com/sagernet/sing-quic v0.7.0-beta.4`；UDP；必须包含 `with_quic` |

本仓库 sing-box 包装模块要求 Go 1.26.0，目前构建工具链为 Go 1.26.4。现有构建标签已经包含 `with_quic`，AnyTLS/Snell 在普通协议注册表中注册。沿用当前 `go.mod` / `go.sum` 和 `-mod=readonly` 即可，无需升级固定内核。

## 协议验收顺序

### AnyTLS

1. TLS 校验开启，服务端证书由测试客户端显式信任；正确密码完成 HTTP 请求和二进制 TCP 回显，校验完整负载。
2. 增加 UDP 经 AnyTLS 的真实回显、多路并发、连接复用，以及取消后的新连接恢复。
3. 对错误密码、错误 SNI、未信任证书分别检查连接失败；对缺少 TLS 和 `tcp_fast_open=true` 检查本地配置拒绝。1.14 源码明确要求出站 TLS，并明确禁止 AnyTLS 的 TCP Fast Open。
4. 最后验证 `idle_session_timeout`、`idle_session_check_interval`、`min_idle_session` 和 client metadata 的保存、导出与运行行为。它们不能只靠分享链接导入成功来验收。

### Snell

1. 首先验证 **v4 客户端连接 v5 服务端**，然后验证 v6 双端。此处的版本差异是上游设计：不支持 Snell v5 的 QUIC proxy 模式，其 TCP 线路协议与 v4 相同，因此上游不提供单独 v4 服务端或 v5 客户端。
2. 验证文档列出的 `none/http` 混淆、错误 PSK、连接复用，以及多用户的服务端 PSK + 客户端 userkey。TLS 混淆代码虽出现在锁定依赖中，仍应单独实测后才作为对外支持结论。
3. v6 验证默认 mode、合法 PSK 边界及错误 mode；不能给 v6 配置继续写 v4 的 obfs 字段。
4. UDP 通过该协议自己的数据通道做回显；不能把“不支持 v5 QUIC proxy”误写为“不支持所有 UDP”。出站代码包含 UDP 转发路径，需实测其负载和关闭行为。

### Hysteria2

1. 现有 `TestCurrentProxyProtocols/hysteria2` 已有真实回环 TCP 请求和旧 `hop_ports` 字段兼容覆盖。阶段 4 增加协议级 UDP 负载、认证失败和取消恢复；现有测试不等同于多端口跳跃已验收。
2. 先验证无混淆和 Salamander，再单独验证 1.14 新增 Gecko，以及 BBR profile 与 QUIC 参数。参数两端必须匹配。
3. 默认 Chrome QUIC 指纹行为与 Ed25519 服务端证书不兼容；沿用现有 ECDSA 证书，避免把证书选择导致的握手失败误判为批测问题。
4. 多端口跳跃需要测试用的回环 UDP 转发器，把不同监听端口转发到同一服务端，从而验证 `server_ports`、`hop_interval`、`hop_interval_max`；不使用宿主防火墙/NAT 重定向规则。
5. 官方 Hysteria2 的 `userpass` 认证以 `username:password` 作为实际密码；sing-box 中应填写该完整字符串。官方程序互通需覆盖这个差异。

Realm 涉及 rendezvous、STUN 和 UDP 打洞，不能由普通回环端到端测试证明可用，应单列后续网络环境验收，不混入基础协议支持结论。

## 原生服务端与官方程序交叉验证

原生双端验证可以证明实际代理链路工作，不能单独证明与所有官方服务端版本兼容。进一步验证可为测试传入外部程序路径及固定版本：AnyTLS 的 `anytls-go`、Surge 官方 Snell server、官方 Hysteria2 server。阶段 4 应记录各程序版本与 SHA-256，使用临时配置和本地端口，不覆盖用户安装。

锁定的 sing-anytls README 声称兼容 `anytls-go`，sing-snell README 说明其 Go 实现目标为匹配官方行为；这些上游声明不替代本项目的交叉实现测试。当前尚未获取或运行上述官方服务端构建。若其平台产物无法直接在 Windows 运行，需要单独的已有测试环境；原生 sing-box 回环路线不依赖这些外部产物。

阶段 4 新测试就绪后，应按以下类别分别记录结果：实际 TCP/UDP 负载、GUI 生成配置、错误凭据、证书模式、取消/重连、节点自动选核。外部程序测试缺少对应二进制时，只跳过该项交叉实现验证，不把它标记为通过。

## 核对来源

- [本项目 sing-box 模块与锁定依赖](../../go/cmd/nekobox_core/go.mod)
- [现有真实协议测试](../../go/cmd/nekobox_core/protocol_test.go)
- [1.14 协议注册表](https://github.com/SagerNet/sing-box/blob/v1.14.0/include/registry.go) 与 [QUIC 构建标签](https://github.com/SagerNet/sing-box/blob/v1.14.0/include/quic.go)
- [AnyTLS 出站实现](https://github.com/SagerNet/sing-box/blob/v1.14.0/protocol/anytls/outbound.go) 与 [服务端文档](https://github.com/SagerNet/sing-box/blob/v1.14.0/docs/configuration/inbound/anytls.md)
- [Snell 版本选项](https://github.com/SagerNet/sing-box/blob/v1.14.0/option/snell.go)、[服务端文档](https://github.com/SagerNet/sing-box/blob/v1.14.0/docs/configuration/inbound/snell.md) 与 [客户端文档](https://github.com/SagerNet/sing-box/blob/v1.14.0/docs/configuration/outbound/snell.md)
- [Hysteria2 服务端文档](https://github.com/SagerNet/sing-box/blob/v1.14.0/docs/configuration/inbound/hysteria2.md) 与 [客户端文档](https://github.com/SagerNet/sing-box/blob/v1.14.0/docs/configuration/outbound/hysteria2.md)

本次实际读取的是 Go 模块缓存中上述 **v1.14.0** 文件，未用滚动更新的 latest 文档代替锁定版本。
