# NekoRay_BY_WanQiu 双内核兼容说明

本地开发分支：`codex/dual-core-latest`。基础版本为官方 3.26，提交 `894410da394aeefbaea0cdee7f29c631c4923698`。

为保留 `rc4-md5` 和跳过 TLS 证书验证，本次将 Xray 固定为 Nekoray 3.26 使用的 **Matsuri Xray v1.8.4** 分支，提交 [`01208225ee7e508044cca8eb6776a117bcecd997`](https://github.com/MatsuriDayo/Xray-core/commit/01208225ee7e508044cca8eb6776a117bcecd997)。这不是最新官方 Xray；仅回退官方模块也不能恢复旧 fork 的全部 Shadowsocks 加密方式。

Xray 使用 Go **1.21.13** 构建，以兼容其 QUIC / REALITY 依赖；sing-box 继续使用 **v1.14.0** 和 Go **1.26.4**。版本和来源提交均已固定，不会在每次构建时自动追随上游。

## 改动位置

```text
Nekoray 3.26 Qt GUI
        │ 原有 gRPC / token 认证接口
        ├── nekoray_core → 配置检查 / 连接记录 → Matsuri Xray 1.8.4
        └── nekobox_core → 旧配置迁移 / 流量统计 → 官方 sing-box
```

| 要修改的行为 | 入口 |
| --- | --- |
| GUI 生成的配置、节点字段、分享链接 | `db/ConfigBuilder.cpp`、`db/`、`ui/` |
| Xray 启停、配置兼容、连接列表 | `go/cmd/nekoray_core/` |
| sing-box 启停、统计、命令行 | `go/cmd/nekobox_core/` |
| 旧 sing-box 配置迁移 | `go/cmd/nekobox_core/internal/compat/` |
| GUI 与内核的共享 RPC 接口 | `go/grpc_server/` |
| 协议握手、传输实现 | 固定提交的 Matsuri Xray / 官方 sing-box 模块 |

GUI 基于 3.26 继续修改，保留双内核选择和原有 gRPC 接口。本次同时重建 Qt GUI 与两个 Go 内核，Windows x64 便携 ZIP 及对应源码 ZIP 输出到 `deployment/packages/`。便携包包含匹配的 Qt / TLS / VC++ 运行库、原版数据库、许可证和文件校验清单，不含个人配置。只替换原版便携包中的内核，不会获得本分支的界面和批测修复。

## 批量 URL 测试

- 固定数量的工作线程消费队列，每个工作线程复用独立 RPC 连接；默认并发数为 10，可在基本设置中设为 1–64。已有配置中的并发数继续保留。
- 节点配置在界面线程生成快照，网络测试并发运行；结果回到界面线程后校验批次、节点 ID、原节点对象和节点内容。取消、删除、替换或编辑节点后，旧结果不会覆盖当前记录。
- 配置构建不再改写 VLESS 的 flow 或正在运行节点的流量统计标签。sing-box 的普通批测配置不再要求无关的 GeoIP / Geosite 数据库存在。
- 修正跨线程使用 QMutex 等待 RPC 的方式，校验 RPC 返回帧，并让本地 RPC 连接直接访问回环地址。单个配置或 RPC 失败不会使整批卡住。
- URL 测试默认超时仍为 10 秒，可单独调整。HTTP 请求统一沿指定内核出站发起；普通模式测量响应头延迟，`truehttp(s)` 模式复用连接，每次采样各有超时预算。
- 停止批测会中止正在等待的 RPC，并停止派发后续节点。此次性能改动针对批量节点测试耗时。

源码与本地回归确认了以上问题和改动；尚未用用户实际节点复现并对照原来的间歇性误判，不能将全部误判归为同一个原因。

## 分组与链接导入

- 主界面右键分组标签可新建、编辑、更新订阅和删除分组。操作使用右键点中的分组 ID；基本组与归档组禁用订阅更新，最后一个分组不能删除，删除仍需确认。
- 粘贴 HTTP / HTTPS 链接后，用三个直接操作按钮选择“作为订阅（添加到该组）”“作为订阅（创建新组）”或“作为链接”。
- 创建新订阅组会打开预填名称、URL 和订阅类型的编辑窗口；确认后保存、选中新组并使用编辑后的 URL 更新，取消不会留下空组或开始下载。
- 分组类型改为互斥的“基本 / 订阅”单选按钮，现有分组保持原有类型锁定行为。
- 下载订阅期间切换分组不会改变导入目标；添加到现有组保持追加语义。解析后的节点和订阅更新结果统一在界面线程写入。

## 配置兼容

配置检查与 sing-box 转换在启动时的内存中完成，不改写已保存的节点配置。替换内核不需要重新导入节点、分组或订阅。

- Xray：原有 3.26 配置直接交给兼容内核，保留 `rc4-md5`、旧 Shadowsocks 插件字段和 `allowInsecure`；跳过证书验证只在配置中显式启用时生效，没有日期限制。连接列表与每个实例的关闭、取消操作仍彼此独立。
- Xray：不再将旧 `domainStrategy` 改写成新版 `targetStrategy`。自定义配置中非默认的 `targetStrategy` 或新版证书校验字段 `pinnedPeerCertSha256` / `verifyPeerCertByName` 会明确报错，避免被旧内核静默忽略。
- sing-box DNS：旧地址形式转换为 typed DNS，支持常见 UDP/TCP/TLS/HTTPS/QUIC/H3、本地解析器、FakeIP 和 `rcode://`；旧 DNS 出站转换为 DNS 劫持规则。
- sing-box 路由：旧 inbound 的 sniff / 域名解析策略转换为规则动作；直接指向 block 的路由转换为 reject，保留供 selector / detour 引用的 block 出站，支持 direct 目标覆盖和 WireGuard endpoint 转换。
- sing-box TUN：旧 IPv4 / IPv6 地址与路由字段转换为新版格式。
- sing-box Hysteria / Hysteria2：旧 `hop_ports` 和数字 `hop_interval` 转换为新版端口范围与持续时间格式。
- GeoIP / Geosite：可读取 3.26 的本地 `.db`，将分类展开成新版匹配字段；缺失数据库或分类时明确报错。数据库内容本身仍是原便携包版本。

## 已验证范围

回归测试运行在 Windows amd64，通过本地回环服务发送真实流量：

- gRPC token 认证、错误配置、启动、停止、重启、重复启动保护。
- SOCKS TCP 和 UDP 转发、URL 延迟测试、流量统计及清零、Xray 连接列表 JSON。
- Xray 的 VLESS TCP、WebSocket、gRPC 转发。
- Xray 的 RC4-MD5 TCP / UDP 转发；自签名证书下 VLESS WebSocket TLS 的跳过验证开关（标准 TLS 和 Chrome uTLS）。正确、错误密码及证书校验成功、失败节点的逐个测试和批量测试判定一致。
- sing-box 的 Shadowsocks、VMess、VLESS、Trojan、Hysteria2、TUIC 转发，其中 Hysteria2 包含旧跳端口配置。
- 新旧配置解析、TUN / FakeIP 配置、旧 sniff 目标覆盖、DNS 转换与 GeoIP / Geosite 匹配。
- 两个真实内核进程的并发 URL 请求、取消传播、超过旧 3 秒限制的请求，以及各自使用不同代理配置的可用、慢、失败、卡住和无效配置节点。
- 真实 Qt GUI → RPC → 内核链路：相同 URL 下逐个测试与批量测试的可用性判定一致；并发设置为 4 时观察到 4 个请求同时运行；取消和节点编辑、删除、ID 复用后的结果归属正确。
- 分组右键菜单、订阅编辑与取消、修改订阅 URL、导入期间切组、已有节点保留和不下载的链接导入。

纯 C++ 调度测试使用 24 个固定等待任务，1 并发约 2.2 秒，8 并发约 0.28 秒。这只验证调度并发能力，不代表实际代理节点的加速倍数。

构建与测试命令见 [Build_Core.md](Build_Core.md)。

## 使用限制

- 尚未连接用户实际节点，也未启用系统代理或实际 TUN 路由；本地测试不等同于各类服务端部署和系统环境均已验证。
- GUI 以 3.26 为基础；本次界面改动未扩展所有新版协议字段，需要时仍使用自定义配置。
- Xray 1.8.4 兼容内核不支持 XHTTP 以及后续 Xray 版本新增的配置能力；已有记录仍保留，但依赖这些功能的节点需要使用支持它们的后端。
- sing-box 中含旧 DNS `outbound` 匹配的自定义配置需要人工迁移为 `domain_resolver`。上游已删除的其他协议或字段也可能需要调整。
- 旧 `sniff_override_destination` 在本包装层运行时保留；由于上游 JSON 不再暴露该开关，CLI `migrate` 对这类配置报错，避免导出的配置改变原行为。
- CLI 保留 GUI 调用需要的 `run` / `check` / `version`，另提供 `migrate`；它不是官方 sing-box CLI 全部子命令的替代品。
- 旧上游更新检查 / 下载接口已禁用，避免覆盖为 4.x。发布自己的 fork 时需设计对应更新源。

源码及组件继续适用各自许可证。对外分发前，应随包保留许可证、注明修改，并提供对应源码。
