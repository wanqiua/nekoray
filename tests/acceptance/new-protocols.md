# AnyTLS、Snell 与 Hysteria2 适配及验收

2026-09-13。本文记录本轮新增协议适配的范围和实际证据。**完整 CTest 7/7 组通过，两种默认内核的 GUI 各 49 项通过；GUI → 实际内核 → 本地服务端链路及错误凭据对照通过。官方服务端交叉互通未测。** Bean、配置构造与真实转发分别计数，最终包按自身哈希独立验收。

## 版本与支持范围

沿用 sing-box **1.14.0**，依赖由 [nekobox_core/go.mod](../../go/cmd/nekobox_core/go.mod) 固定。Matsuri Xray 保持 **1.8.4**、提交 `01208225ee7e508044cca8eb6776a117bcecd997`，继续保留 RC4-MD5 和 `allowInsecure`。本轮不通过升级 Xray 实现这些新协议，也不改写用户原安装和配置。

| 协议 | 当前适配项 | 约束及内核选择 |
|---|---|---|
| AnyTLS | 密码、TLS/SNI、证书校验开关、自定义 PEM CA、ALPN、uTLS 指纹；空闲检查间隔、会话超时、最少空闲会话数、客户端元数据 | 使用 sing-box；Xray 不支持。密码必填，TLS 必须开启；两项自定义空闲时长须大于 5 秒，留空使用上游 30 秒默认值。当前上游拒绝 TCP Fast Open |
| Snell 4 | PSK、用户密钥、连接复用、TCP/UDP 选择、无混淆或 HTTP/TLS 混淆及混淆域名 | 使用 sing-box；客户端版本必须为 4。原生互通夹具使用 **v4 客户端 → v5 服务端**，两者的 TCP 线路协议兼容；这不包含 v5 的 QUIC proxy 模式 |
| Snell 6 | PSK、用户密钥、连接复用、TCP/UDP 选择；默认、`unshaped`、`unsafe-raw` 流量整形 | 使用 sing-box；客户端/服务端版本均为 6。PSK 为 12–255 个 UTF-8 字节，用户密钥不超过 255 字节；不接受 v4 混淆字段 |
| Hysteria2 | 保留旧密码、带宽、TLS、跳端口及窗口字段；新增 BBR profile、Gecko、Chrome QUIC 指纹开关、跳端口最小/最大间隔 | 使用 sing-box 原生支持新选项；Xray 不支持。非标准 BBR、Gecko、关闭 Chrome 指纹或随机跳端口不能交给旧的外置核心 |

AnyTLS 的 UDP 通过协议的数据通道转发；Snell 同样支持 UDP 转发。“不支持 Snell v5 QUIC proxy”不能解释成“不支持 Snell UDP”。仅 UDP 的 Snell 节点显示 HTTP URL 测试“不适用”，不能据此归类为不可用节点。

Hysteria2 的选项语义如下：

- BBR profile 支持空值/默认、`standard`、`conservative`、`aggressive`。上、下行带宽为 0 时使用 BBR；在编辑器选择 profile 不自动修改已有带宽。
- 混淆支持默认推断、关闭、Salamander、Gecko。旧配置未指定类型时，有混淆密码就沿用 Salamander；显式关闭时不能遗留混淆密码。
- Gecko 包长满足 `1 ≤ 最小值 ≤ 最大值 ≤ 2048`；0 使用 512/1200 默认值。自定义包长仅用于 Gecko，切换混淆类型不会在编辑时悄悄清空输入。
- `disableChromeParrot` 默认关闭，即继续模拟 Chrome QUIC 指纹。原生互通测试证明：使用 Ed25519 服务端证书时，关闭该模拟后可以连接，保持默认模拟则握手失败。
- 跳端口可输入逗号分隔的单端口、`起始-结束` 或 `起始:结束`。Bean 保留原文本，发给 sing-box 时生成 `server_ports` 的 `起始:结束` 列表，单端口也带冒号；使用跳端口时不再生成单一 `server_port`。
- 旧的整数 `hopInterval` 仍按秒保存。新的最小间隔留空时继承旧值；最大间隔留空时保持固定间隔。有效最小间隔须至少 5 秒，最大值不得小于最小值；随机间隔不能以 0 作为最小值。固定间隔 0 使用上游 30 秒默认值。

## 导入、分享与保存约定

入口包含 `anytls://`、`snell://`、`hy2://` / `hysteria2://`、完整 `nekoray://<type>#<Base64URL JSON>` 及已适配字段的 Clash/YAML 订阅。**`snell://` 是本项目兼容语法，不是官方 Snell URI 标准**；须显式指定 `version=4` 或 `version=6`。

链接中的密码、PSK、名称和查询参数按组件解码一次，保留冒号、字面 `%`、`+`、Unicode 及转义的 `&`/`=`。原生 URI 无法无损表示的高级字段使用完整 Nekoray 分享。完整格式是节点 Bean 数据，不能夹带分组信息、测试结果或本机内核偏好。

未知字段、冲突参数、错误类型、整数溢出、非法协议参数应明确拒绝，不能静默丢弃后生成一个不同节点。订阅更新先解析到暂存区；遇到被拒绝的条目不应以残缺结果覆盖原分组、节点顺序或本地元数据。普通粘贴允许单独导入有效条目，并报告无效条目；这与订阅整体替换的提交条件不同。

这些入口由 [new_protocol_regression_test.hpp](../new_protocol_regression_test.hpp) 验收。严格端口和带宽解析、重复 YAML 字段拒绝、旧混淆类型分享保留、混合有效/无效订阅保护均已随完整 GUI 回归通过。下表中的独立 Bean 检查不替代这些集成用例。

Qt 6 版本的 64 位整数加载和 HY2 完整分享检查已改用整数转换；`2^53+1` 和 `qint64` 最大值在编辑、磁盘重新加载及完整分享回导中精确保留。Qt 5 仍沿用原来的 double 路径，不承诺相同的极值精度。原始完整 JSON 的重复键采用 Qt 的解析结果，不承诺检测原始文本中的重复键；原生 URI 与 YAML 的重复字段会被明确拒绝。

## 已取得的证据

下列日志位于本地 `build/`，不是发布包的一部分。“通过”仅对应产生日志时的测试源码和配置快照。

| 检查层级 | 记录结果 | 范围与证据 |
|---|---|---|
| AnyTLS / Snell Bean | **130/130 检查通过** | 独立 QtCore/QtNetwork 控制台夹具编译实际 Bean 实现；覆盖字段序列化、URI/完整分享、百分号与 UTF-8、时长及密钥字节边界、协议版本/选项冲突、Xray 明确拒绝和 sing-box 配置映射。日志：`build/dynamic-core-check/protocol-bean-check/results.log`；夹具范围见同目录 `README.txt` |
| Hysteria2 Bean | **191/191 检查通过** | 基于当时暂存的完整生产源文件，覆盖旧字段、新选项、分享、拒绝无效输入且不修改目标 Bean、发核配置。日志：`build/dynamic-core-check/hysteria2-bean-work/results.log`；不是 GUI 或订阅集成测试 |
| Hysteria2 时长参考 | **79 个上游参考用例** | 与锁定上游 `hysteria.NewHopConn` 的判断比较；合成 dial 不打开套接字。参考及生成日志：`hysteria2-bean-work/duration-reference.json`、`duration-reference.log`，比较纳入上述 Bean 夹具 |
| Hysteria2 原生配置构造 | **16/16 配置通过** | 将 Bean 生成的配置交给原生 sing-box `Box.New/Close`，**没有调用 Start**。覆盖各 BBR profile、混淆、Gecko 默认/边界、Chrome 开关和跳端口。日志：`hysteria2-bean-work/native-results.log` |
| HY1 / TUIC 兼容快照 | **修改前后相同** | 序列化、分享、核心配置及外置核心选择的快照一致；`hysteria2-bean-work/snapshot-comparison.log`。快照 SHA-256：`6582f14f300cc85d50723258669cb8b317bf5d7ee1c875588d32455fee4e96a2` |
| Go 协议与约束回归 | **62 个叶子用例通过** | 8 个顶层测试中的 60 个子用例，加上没有子用例的 Ed25519 与实际跳端口测试。合法线路进行真实回环握手和负载转发；部分非法选项用例只验证构造或拨号拒绝。日志：`build/dynamic-core-check/protocol-peers-final.log` |
| 仅 UDP 的 Snell URL 分类 | **v4、v6 两例通过** | Go URL probe 返回不支持，避免误记连接失败；`build/dynamic-core-check/protocol-probe-network.log` |
| C++ 静态编译检查 | **通过** | Bean 生产编译参数检查日志：`protocol-bean-syntax.log`、`hysteria2-bean-work/syntax-production.log`；HY2 编辑器及测试头文件独立 MSVC `/Zs` 日志：`hysteria2-editor-check/syntax.log`。语法检查不证明界面或保存行为通过 |

表中未写完整前缀的日志都位于 `build/dynamic-core-check/` 下。早期试验日志不与最终日志重复计数。

Go 回环互通的具体覆盖：

- **AnyTLS**：默认及高级空闲会话/元数据选项，TCP HTTP 完整响应负载、UDP 回显（含 8192 字节）、真实承载连接复用；错误密码、未信任证书、错误 SNI 拒绝；缺失 TLS、启用 TCP Fast Open 的配置拒绝。
- **Snell**：v4 的 none/HTTP/TLS 混淆，v6 的三种整形模式，分别测试复用开关；TCP HTTP、UDP 回显、多用户 userkey、错误 userkey/PSK，以及 v6 PSK 的 12/255 字节正例与越界反例。TCP/UDP 限制通过正常路由入口测试。
- **Hysteria2**：默认及三种 BBR profile、Salamander、Gecko 默认和自定义包长、Chrome 模拟开关；TCP HTTP、UDP 回显（含 4096 字节）、密码/混淆密码/SNI 反例、Ed25519 证书组合。BBR 用例只证明这些配置能握手和传输，不是拥塞算法性能对比。
- **实际跳端口**：64 个测试 UDP 转发端口通向同一回环服务端；配置最小 5 秒、最大 6 秒，观察到目的端口和源套接字在启动稳定后发生变化，变化前后都继续验证传输。最终日志记录约 5.101 秒发生变化。测试允许 1 秒调度/观察误差，不证明随机分布或长时间跳跃稳定性。

## GUI 与完整链路验收

完整回归日志为 `build/dynamic-core-check/protocol-roundtrip-ctest.log`，7/7 组通过，35.65 秒。两种默认内核下均为 49 项通过、0 失败；普通 CTest 跳过的两个主机用例和一个协议用例需要分别显式运行。显式空 BBR 值已通过按字段保留空字符串修复，仅该字段启用新序列化标志；克隆、磁盘重载和分享用例均已复验。

| 项目 | 用户可见预期 | 入口 / 当前状态 |
|---|---|---|
| 创建、编辑与选核 | 新协议可新建、编辑、分享；自动选择 sing-box，Xray 选项说明不支持。修改内核偏好仅在保存后用于后续连接，编辑预览不改变当前连接 | `newProtocolsResolveCoreAndEditorChoices`；**已通过** |
| AnyTLS 编辑 | 密码默认遮挡，可显式显示；高级区默认收起。取消不提交，40,000 字符元数据、原始 CRLF 证书和会话参数保存完整 | `anyTlsEditorDraftAndAdvancedSave`；**已通过** |
| Snell 编辑 | 版本 4/6 选择明确，显示对应版本字段；密码可按需显示，高级区默认收起；切换版本时保留编辑草稿，保存只提交该版本支持的选项 | `snellEditorVersionDraftsAndSave`；**已通过** |
| Hysteria2 编辑 | 高级区默认收起；空值/显式默认/未知导入值不被替换；选择 BBR 不修改带宽；64 位接收窗口、长密码不截断；预览不弹校验框，非法范围在保存时阻止提交 | 三个 `hysteria2*` GUI 用例；**已通过** |
| 订阅与分享 | 已适配字段完整往返；错误类型、溢出及不支持选项被拒绝；部分无效的订阅不覆盖旧配置，名称/顺序变化仍保留本地内核偏好与测试来源 | 四个新协议导入/订阅 GUI 用例；**已通过** |
| GUI → 配置 → 实际内核 | 7 个服务端配置经过真实编辑对话框保存、分享及重新导入；完整 ConfigBuilder 和 sing-box SOCKS 入口验证 TCP/UDP；三种协议错误凭据的 TCP、Snell UDP 负对照及恢复正确 | `newProtocolPeerRuntimeAcceptance`；**已通过** |
| 新协议批量 URL 测试 | 7 个正确节点成功；仅 UDP 的 Snell 不适用；3 个错误凭据节点连接失败。结果来源、并发设置 4、当前连接保持及删除条件正确 | 同一 runtime 用例；**已通过** |

HY2 编辑器测试由 [hysteria2_editor_test.hpp](../hysteria2_editor_test.hpp) 提供；完整链路由 [protocol_gui_runtime_test.hpp](../protocol_gui_runtime_test.hpp) 提供。它们使用测试数据库与合成回环节点，不读取用户节点，不启用系统代理或 TUN。

配套 [TestProtocolGuiPeers](../../go/cmd/nekobox_core/protocol_gui_fixture_test.go) 提供 7 个真实回环服务端：AnyTLS、Snell 4 无/HTTP/TLS 混淆、Snell 6、Hysteria2 Salamander/Gecko。显式设置 `NEKORAY_PROTOCOL_GUI_FIXTURE` 为**全新的绝对 manifest 路径**才运行；Go 测试超时须至少 210 秒。服务端 manifest 的 `schema=1`，有效期 180 秒；创建 `<manifest>.stop` 可提前结束，下一次使用新路径。清单中的 CA 是测试 PEM 内容，不导入系统证书库。未设置该环境变量时 fixture 和 GUI 用例会跳过；跳过不算验收通过。

实际报告位于 `build/dynamic-core-check/protocol-gui-manual-20260913/`：QTest **3/3 通过，0 失败，0 跳过，11.288 秒**，GUI 和 fixture 退出码均为 0。UDP 负载分别为 AnyTLS/Snell6 8192 字节、HY2 4096 字节、Snell4 1200 字节。测试程序 SHA-256：`2ac2b9975a980c405bb94742de3db7243264e59ffeda5c4360900b5e19446ece`；fixture SHA-256：`604855a94ccc464ce56ceace33d31dc0f09ea1a97299adac2e22151ef4ae1a81`。配套 [protocol-gui.ps1](protocol-gui.ps1) 提供独立编译、唯一目录、按进程句柄清理及环境恢复的可复现入口。

该驱动的源码编译路径也已实际通过，报告为 `build/dynamic-core-check/protocol-gui-driver/run-20260913-110158-643-8c597e96d70b42cb9d7f1fa91082e89f/result.json`，QTest 3/3 通过，11.668 秒，编译/GUI/fixture 均正常退出，进程环境已恢复。最终测试程序 SHA-256 为 `effe97e01c68e8541744d90e3f7d16e57a0bfeb048f23005e7d3b5eb81a9422f`。

第二次驱动试运行曾在“7 个成功节点必须恰好产生 7 次 HTTP 请求”的断言失败。现有 `startupRetryTransport` 可在启动 2 秒内、明确本地连接重置且原请求未超时时重试一次 GET，7 个节点实测可能收到 8 次请求。最终用例据此检查总请求数在 7–14 之间，继续严格验证每个节点的状态、来源、指纹、错误凭据阻断和当前连接；生产重试逻辑没有改动。最终轮实际为 8 次请求，所有结果分类正确。

## 证据边界与后续交叉验证

当前互通测试的客户端与服务端均来自同一份锁定的 sing-box 及其依赖：`sing-anytls v0.0.11`、`sing-snell v0.0.0-20260829071736-20f2eaec77c3`、`sing-quic v0.7.0-beta.4`。测试使用独立实例和真实套接字，能发现实际握手、认证、封包与转发错误；但同一实现的两端可能共享相同假设，不能据此证明与独立实现或所有版本互通。

| 独立服务端 | 当前状态 | 后续应记录 |
|---|---|---|
| AnyTLS 官方 `anytls-go` | **未测** | 固定版本/提交和程序 SHA-256；基础及高级会话配置、证书正反例、TCP/UDP |
| Surge 官方 Snell server | **未测** | 官方程序版本/哈希、v4 客户端对应服务端版本、混淆/多用户/复用/UDP；v6 另列，不由 v4 结果推断 |
| 官方 Hysteria2 server | **未测** | 程序版本/哈希、认证方式、证书、TCP/UDP、可共同支持的混淆与跳端口；`userpass` 认证应使用完整 `username:password` 作为密码 |

Gecko、BBR profile 等 sing-box 选项不能默认视为上述官方服务端支持项，应按实际服务端版本逐项核对。Realm/STUN/UDP 打洞不在本轮回环验收范围内。回环测试也不提供公网吞吐、丢包、漫游、长期会话或三种协议各自取消后重连的完整结论；需要这些结论时应补充对应场景并单独记载。

Windows 系统代理、TUN 和 UAC 的记录见 [system-host.md](system-host.md)，包生命周期记录见 [package_acceptance.md](package_acceptance.md)；两者分别验收，不能代替本页的新协议 GUI 链路证据。实际内核 SHA-256 固定于 `deployment/windows64/core-versions.json`，发布包会再次核对与 EXE 一致。

## 实现与测试来源

- [AnyTLS Bean](../../fmt/AnyTLSBean.cpp)、[Snell Bean](../../fmt/SnellBean.cpp)、[QUIC Bean](../../fmt/QUICBean.hpp)。
- [Hysteria2 配置与校验](../../fmt/Bean2CoreObj_box.cpp)、[链接导入](../../fmt/Link2Bean.cpp)、[分享](../../fmt/Bean2Link.cpp)、[订阅导入](../../sub/GroupUpdater.cpp)。
- [Go 协议互通/约束用例](../../go/cmd/nekobox_core/protocol_peers_test.go)、[正常路由入口的网络限制用例](../../go/cmd/nekobox_core/protocol_routing_test.go)、[仅 UDP 的 URL 分类用例](../../go/cmd/nekobox_core/probe_network_test.go)。
- [前期服务端研究](batch-protocol-server-research.md) 是开发前的路线文档；其中“尚未实现”的描述属于当时状态，本页按当前代码和分层证据更新进度。
