评估及复核日期：2026-09-13。对象是本地 Nekoray 3.26 二开版本，以及已交付的 Matsuri Xray 1.8.4 兼容内核和 sing-box 1.14.0。本报告记录技术事实及边界，实施范围与用户操作以复核后的[开发计划](C:/Users/zqf/Documents/aaa/nekoray/docs/Dynamic_Core_Development_Plan.md)为准。本轮只更新评估与计划，没有修改业务代码、替换内核或改写用户配置。

**结论：逐节点手动/自动选核可行。首个正式版本应一起完成不重启界面的切换、混合节点批测、订阅偏好持久化、异常恢复和基础 TUN 切换。已有 TCP、UDP、QUIC 会话不能直接从一个内核迁移到另一个内核，普通切换会产生重连。**

2026-09-13 实际访问 [上游 latest 发布入口](https://github.com/SagerNet/sing-box/releases/latest)，最终指向 [v1.14.0](https://github.com/SagerNet/sing-box/releases/tag/v1.14.0)。当前已经使用这个稳定版本，接下来的主要工作是把内核功能接入节点模型、导入器、编辑窗口和运行管理。

本次同时阅读了固定版本的上游源码、随源码提供的文档及本项目的构建标签，并对当前便携包中的 sing-box 做了 16 组配置检查。检查使用虚构凭据和回环地址，仅调用 CLI 的 check，不启动代理，不连接用户节点。9 组成功构造配置，7 组得到预期的不支持错误。**配置检查证明功能已编译、字段可解析；新协议的真实服务端互通仍需开发阶段验证。**

检查记录：[results.json](C:/Users/zqf/Documents/aaa/nekoray/build/core-feature-assessment-20260913/results.json)。被检查的 sing-box SHA-256 为 4305567fa5e5b5f8093fa921a51b5b20b08554b21f289b6df5e768f95dba8b8d，与安装目录的版本清单一致。

**适合接入界面的协议**

| 协议或功能 | 当前内核与界面情况 | 适配工作与建议 |
| --- | --- | --- |
| AnyTLS | 自 sing-box 1.12 引入；当前二进制配置检查通过，GUI 没有专用节点类型 | 选核完整流程稳定后首批新增，编辑、导入导出和测试一起接入；会话复用等高级项折叠；仅由 sing-box 原生承载 |
| Snell | 1.14 新增；当前二进制的 version=4 和 version=6 配置均通过，GUI 未适配 | 选核完整流程稳定后首批新增；配置版本为 4/6，普通 v5 按 v4 配置，服务端互通仍待验证；v5 QUIC 模式不支持 |
| Hysteria2 | 已有 GUI、导入和转发支持，不必新增另一种重复节点 | 补 1.14 的 BBR profile、随机跳端口间隔、gecko 混淆及 Chrome QUIC 指纹开关；Realm/NAT 穿透需服务器配合，可后续接入 |
| SSH | 当前二进制配置检查通过，GUI 未适配 | 可以新增用户、密码/密钥、主机密钥校验和算法选项；明确 TCP 能力，不应表现成完整 UDP 节点 |
| ShadowTLS v3 | 当前二进制配置检查通过，GUI 未适配 | 与上层 Shadowsocks 等组合使用，需设计组合节点和 detour，不能仅把 ShadowTLS 当作普通 SS 节点解析 |
| WireGuard | sing-box 已编译 with_wireguard，兼容层可迁移旧出站；Matsuri Xray fork 也包含 WireGuard | 可做支持双核共同参数的专用编辑器；sing-box 1.14 使用 endpoint，两边结构与行为需分别验证，不能永久归为 sing-box 专属 |
| NaiveProxy 内置出站 | 上游从 1.13 提供；当前二进制明确返回未编译 with_naive_outbound；现有 Naive 节点依赖外部程序 | 后续启用构建标签，并在 Windows 包中加入匹配的 libcronet.dll；同时处理现有外部 Naive 配置的兼容 |
| OpenVPN / OpenConnect | 1.14 新增；当前二进制分别明确返回缺少 with_openvpn / with_openconnect | 需要启用构建、endpoint 配置、证书/密钥或交互认证、DNS/路由和状态界面；属于较大的一组功能 |
| Tailscale | 上游支持，当前二进制明确返回缺少 with_tailscale | 需要启用构建、登录授权、状态持久化、出口节点和路由/DNS；建议独立阶段实施 |

Hysteria、TUIC、Shadowsocks、VMess、VLESS、Trojan、SOCKS、HTTP 已有相应界面或适配逻辑，不应重复列为本次全新协议。Hysteria 的非标准 UDP 模式、强制外部核心等情况仍需单独判定。

**除协议之外，值得接入的 sing-box 功能**

| 功能 | 对这个客户端的用途 | 适配边界 |
| --- | --- | --- |
| 1.14 DNS 响应评估、并行评估、乐观缓存、查询超时 | 改进 DNS 策略编辑和异常排查 | 现有迁移层只是保留旧配置；新功能需要原生字段和界面，不能宣称自动解决所有节点测速问题 |
| 原生 rule-set、规则动作及新版 TUN DNS/NAT 设置 | 更新分流管理、DNS 劫持与 VPN 配置 | 按 Windows 平台能力测试；保留旧规则与现有 geodata 兼容 |
| selector / urltest | 节点组选择、组内自动选节点，后期可用于稳定入口切换 | 它们只管理一个 sing-box 实例内的出站；要切换外部 Xray，仍需要进程管理和桥接 |
| 1.14 API 服务、Dashboard、连接观测 | 更完整的日志、出站控制和诊断面板 | 当前 NekoRay 自有 gRPC 不等于上游新 API，现有包装程序也没有完整的 sing-box api CLI；需适配接口或通过服务接入 |
| TLS/ECH、指纹、握手超时等扩展 | 为支持这些能力的协议增加选项 | 按协议、TLS 实现和构建能力显示；例如 Naive 的 TLS 选项范围与普通 VLESS 不同 |

Hysteria2 的 Chrome QUIC 指纹模拟在 1.14 默认启用；上游明确说明 Ed25519 证书可能因此握手失败，需要对应的 disable_chrome_parrot 选项。它与“跳过证书验证”不是同一个设置，应在协议编辑中正确区分。该限制是上游说明，不代表已经定位到用户之前的批测问题。

USB/IP、服务端证书签发、服务端 API 托管和 Linux 网络命名空间等也属于上游新功能，但与当前 Windows 代理节点客户端的主要需求关系较小，不建议放进首批适配。

**按当前两个内核判断的选核示例**

这里的“支持”是当前固定版本与实际构建的能力，不是最新官方 Xray 的能力，也不包含额外外部程序提供的协议。

| 节点组合 | Matsuri Xray 1.8.4 | 当前 sing-box 1.14.0 | 建议行为 |
| --- | --- | --- | --- |
| 常规 SS / VMess / VLESS / Trojan | 常见组合支持 | 常见组合支持 | 核心和 GUI 都能完整表达参数后可手动选择，自动模式继承默认优先内核 |
| 普通 SOCKS、HTTP CONNECT / HTTPS | 核心支持，GUI 的 SOCKS4 版本生成需修正 | SOCKS 无原生 TLS/WS/gRPC 出站配置；HTTP 有 TLS，但没有 V2Ray transport，且仅承载 TCP | 按 SOCKS 版本、TCP/UDP 和 TLS/传输判定，未修复验证前不把所有组合标为双核兼容 |
| SS rc4-md5 | 支持，已有回归验证 | 支持，本次配置检查通过 | 两个内核都允许；不固定分配给 Xray |
| SS bf-cfb | fork 有对应实现 | 不支持，本次配置检查明确拒绝 | 可作为 Xray 专属兼容示例，开放前补实际转发回归 |
| 普通 VLESS WS + TLS + 跳过证书验证 | 支持，已有回归验证 | 配置检查通过，已有 TLS insecure 字段映射 | 两个内核都允许；沿用原节点选项 |
| VLESS / VMess 使用 mKCP | 内核支持；GUI 的 seed/header 等参数覆盖需要补齐 | 不支持，配置检查拒绝 kcp | 仅完整保留配置与安全语义的组合可选 Xray；不能只凭 network=kcp 判断 |
| VLESS / VMess 使用 HTTPUpgrade | 当前旧 fork 的传输列表没有该类型 | 支持，VLESS 配置检查通过 | Host/path、TLS/ALPN 和 flow 等也有效时选 sing-box |
| V2Ray QUIC transport | 支持部分 Xray 专属参数 | 没有额外加密等同等选项 | 仅共同参数子集允许切换；不得静默丢弃密钥、加密或 header 参数 |
| WireGuard | fork 已包含 | 当前构建已包含 endpoint 实现 | 两边模型与共同参数验证后可提供手动选择 |
| Hysteria / Hysteria2 / TUIC | 无原生实现，旧 GUI 可尝试外部核心 | 常用模式已有支持 | 优先 sing-box；外部程序路径另行管理 |
| AnyTLS / Snell / ShadowTLS / SSH | 没有对应原生实现 | 已包含，相应配置检查通过 | 接入 GUI 后自动选 sing-box |
| VLESS XHTTP | 不支持 | 不支持，配置检查明确拒绝 xhttp | 显示两个内核均不支持；不能通过自动选核恢复能力 |
| OpenVPN / OpenConnect / Tailscale / 内置 Naive | 当前 Xray 不提供对应原生能力 | 当前构建未启用 | 显示缺少能力，不能误判为另一个内核可用；扩展构建后再开放 |
| 自定义完整 JSON、专属补充 JSON、JS 配置钩子、代理链 | 取决于配置来源及每一跳 | 取决于配置来源及每一跳 | 绑定配置所属内核；组合无法保持语义时给出原因 |

能力判定应覆盖协议、加密方式、传输、TLS/REALITY、flow、指纹、插件、UDP、mux、endpoint、编译标签、自定义配置以及当前代理/TUN 模式和 DNS/路由设置，并同时检查 GUI 适配器能否保留这些参数。比如两边都有 VLESS，不代表全部 VLESS 节点能互换；配置解析通过也不等于两边的流量行为完全相同。

复核发现，[Xray SOCKS 生成器](C:/Users/zqf/Documents/aaa/nekoray/fmt/Bean2CoreObj_ray.cpp:76) 未写入 SOCKS4 的 settings.version，不能因此误认为 Xray 内核不支持 SOCKS4。[sing-box 共用流设置生成器](C:/Users/zqf/Documents/aaa/nekoray/fmt/Bean2CoreObj_box.cpp:81) 也不能把 TLS 或 WS/gRPC 字段无条件用于 SOCKS/HTTP。mKCP 的 REALITY 等组合须按实际传输实现拒绝不受支持的安全语义，不能依赖解析通过就放行。

**建议的交互规则**

1. 旧节点和新导入节点缺省自动，默认优先内核沿用现有选择。普通使用继续导入、双击连接和批测，不需要先按内核分组。
2. 主界面选中节点旁提供“自动 / Xray / sing-box”互斥按钮，右键支持批量设置。单节点操作中，未运行节点只保存偏好，正在使用的节点点击后执行切换；批量设置一律只保存下次偏好，保持当前连接。不要求进入基本设置或重启软件。
3. 两边完整支持时尊重手动偏好；只有一边支持时自动选它、禁用无效选项并就近解释。保存偏好暂时失效时要标注，不把偏好与实际运行状态混为一谈。
4. 未运行节点显示预计内核，运行区显示确认启动的实际内核，延迟标注当次测试内核。两边均不支持时明确原因，保留节点。
5. 目标启动失败时恢复原运行上下文；没有更新选择时恢复原界面选择，有更新选择时先完成恢复再处理最新意图，旧回调不能覆盖新选择。恢复失败如实显示断开。网络超时、认证和证书失败不触发后台反复换核；可提供不改变当前连接的“测试另一内核”操作。
6. 默认每个节点只测一个内核，两个内核共用总并发上限。混合批测、历史结果、排序和“移除不可用”作为同一功能交付。
7. 自动选择不修改节点加密、TLS 验证和 flow 等原始值，不做无依据的专属 JSON 转换；正常切换不弹重复确认。详细的操作对象、失败提示和验收见[开发计划](C:/Users/zqf/Documents/aaa/nekoray/docs/Dynamic_Core_Development_Plan.md)。

**当前代码为什么需要改造**

目前基本设置中的切换逻辑写入 groups/coreType 并重启整个程序，见 [dialog_basic_settings.cpp](C:/Users/zqf/Documents/aaa/nekoray/ui/dialog_basic_settings.cpp:221)。[NekoGui.hpp](C:/Users/zqf/Documents/aaa/nekoray/main/NekoGui.hpp:11) 中的 coreType 和 IS_NEKO_BOX 是全局状态。

[main.cpp](C:/Users/zqf/Documents/aaa/nekoray/main/main.cpp:162) 只加载其中一份设置：groups/nekoray.json 或 groups/nekobox.json；路由目录也分为 routes / routes_box。[mainwindow.cpp](C:/Users/zqf/Documents/aaa/nekoray/ui/mainwindow.cpp:400) 只准备一个主内核进程和一组 RPC 端口/token。

[ConfigBuilder.cpp](C:/Users/zqf/Documents/aaa/nekoray/db/ConfigBuilder.cpp:57) 根据全局值生成配置；[mainwindow_grpc.cpp](C:/Users/zqf/Documents/aaa/nekoray/ui/mainwindow_grpc.cpp:43) 的启动、测试使用 defaultClient；[TrafficLooper.cpp](C:/Users/zqf/Documents/aaa/nekoray/db/traffic/TrafficLooper.cpp:26) 也从这个客户端查询统计。节点的延迟目前只存一个数值，见 [ProxyEntity.hpp](C:/Users/zqf/Documents/aaa/nekoray/db/ProxyEntity.hpp:32)。

另外，CoreProcess 的退出、自动重启和 token 也使用全局状态；部分 VPN 重置逻辑按进程名处理 nekobox_core。双进程后必须改成实例/PID 所有权管理，避免关闭另一个 RPC 内核、TUN 辅助进程或用户在其他目录运行的实例。

因此应让目标内核随配置、任务和会话显式传递。不能在测速工作线程内来回修改 coreType，否则会重新引入配置串用、测试发错内核和结果错写。

复核还发现三个直接影响首版便捷性和数据保留的现有问题：

- [测试写回](C:/Users/zqf/Documents/aaa/nekoray/ui/mainwindow_grpc.cpp:225) 将多种错误统一写成负延迟，[移除不可用](C:/Users/zqf/Documents/aaa/nekoray/ui/mainwindow.cpp:1482) 按负延迟删除。能力不支持、核心异常、取消和过期结果必须单列，不能让它们成为删除候选。
- [订阅更新](C:/Users/zqf/Documents/aaa/nekoray/sub/GroupUpdater.cpp:602) 有解析前删除全部旧节点的分支，且[首项匹配](C:/Users/zqf/Documents/aaa/nekoray/sub/GroupUpdater.cpp:654) 使用 deleted_index > 0。必须先保留本地元数据、解析和匹配后再提交，并覆盖首项、重排、重名及清空更新场景。
- [共享 RPC](C:/Users/zqf/Documents/aaa/nekoray/go/grpc_server/gen/libcore.proto:6) 当前没有 Check/Capabilities 接口。预检与能力发现需要明确新增，不能把它们当成已有功能。

**建议先实现的运行方式**

引入 CoreManager，为两个内核维护各自的进程、RPC 端口、token、状态、重启策略和客户端。需要某个内核时启动对应后台进程，已有进程可以复用；两个进程可以同时提供控制和测试服务。

目标内核、公共设置快照、内核专属设置、DNS/路由、TLS 策略和代理/TUN 模式一同传入配置生成、预检和实际启动。预检只证明本地配置可构建且能力匹配，不证明远端连接可用；新接口不得占用公共监听或改变系统路由。

~~~mermaid
flowchart TD
    Node["节点配置与手动偏好"] --> Capability["按版本、构建和参数判断能力"]
    Capability --> Manager["CoreManager：管理两个独立实例"]
    Manager --> Ray["Xray 进程与 RPC 客户端"]
    Manager --> Box["sing-box 进程与 RPC 客户端"]
    Ray -.-> Listener["正常代理入口只由当前选中的内核监听"]
    Box -.-> Listener
~~~

普通代理模式保留用户原有的监听地址、端口和系统代理设置。切换流程为：解析选核意图 → 准备目标核心进程并确认 RPC 就绪 → 核实能力、构建并预检目标配置 → 停止旧代理实例并释放监听 → 启动目标代理 → 更新界面状态。预检失败时保留旧代理运行；启动失败时恢复旧配置，并报告失败原因。恢复旧配置不能复活已经断开的连接。

批量 URL 测试按每个节点选择内核，不改变当前正在使用的代理。每个工作线程按内核持有独立测试客户端，任务携带节点快照、内核、版本/进程代次和配置摘要；取消、改节点、改偏好、删除、内核重启后，旧结果不能覆盖新状态。结果同时绑定实际测试配置、相关设置、URL/模式参数和内核构建；主列表只显示下次连接上下文的有效结果。当前连接测试单独标注，来源不明的旧延迟和已失效结果只作历史，不参与当前排序和不可用删除。

数据迁移增加可选的本地节点偏好字段，旧数据缺省为自动；当前 coreType 用作初始默认优先内核。保留原节点、分组、订阅、两套核心设置和路由文件。偏好不参与协议配置的匹配键；订阅更新先可靠匹配同一节点再提交，无法对应时回到自动而不串配。保留旧设置文件不等于切换行为一致，还需要前述明确的当前运行上下文；自定义配置按原来源绑定内核。

**基础 TUN 纳入首版，保持旧连接作为后续增强**

| 目标 | 可行性 | 成本与限制 |
| --- | --- | --- |
| 普通代理模式，逐节点切换，不重启 GUI | 高，可先内部实现 | 与混合批测、订阅及基础 TUN 一起构成首个完整版本；切换时已有会话会重连 |
| TUN 模式下切换，保持 DNS、分流和系统入口一致 | 可行，应在新增协议前完成 | 优先复用既有辅助进程；由程序处理必要的转换/重启，避免要求用户手动关开 TUN |
| 切换后新连接走新内核，旧连接继续使用旧内核 | 有条件可行 | 稳定前端入口、同时保留旧实例、排空旧连接和实例回收；不是把旧会话迁移到新内核 |
| 将已建立的 TCP/TLS/QUIC 会话直接转移到另一个内核 | 不可作为通用功能承诺 | 两个内核的协议栈、握手、加密状态互不共享 |

当前已经有独立的 sing-box TUN → 本地 SOCKS 路径，见 [WriteVPNSingBoxConfig](C:/Users/zqf/Documents/aaa/nekoray/db/ConfigBuilder.cpp:1059) 和 [TUN 模板](C:/Users/zqf/Documents/aaa/nekoray/res/vpn/sing-box-vpn.json)。首版优先复用这条路径，并处理现有 sing-box 内置 TUN 的转换或受控重启。无需为基础切核先新建一整套 selector 前端；按实例/PID 管理进程是前置条件。

基础 TUN 必须覆盖用户原有开关、权限、DNS、FakeIP 和路由意图。切换可以有短暂重连，但不能自动关闭 TUN 或放宽规则来绕过兼容问题。未验收 TUN 的版本只能作为普通代理预览。

后续若需要让旧连接继续运行，可增加长期稳定入口和出站 selector，并同时保留仍有连接的旧实例。SOCKS 桥接的 UDP、域名/FakeIP、DNS、分流、mux、统计和性能需要逐项验证，不能假定与原生直连完全等价。上游 selector 的 interrupt_exist_connections=false 也不是“所有内部连接都不中断”的保证。

**建议的实施顺序与验收**

1. 先完成能力及适配器边界、公共/内核专属设置上下文、预检接口、进程所有权和外部 TUN 复用方案。测试协议参数不能被静默丢弃。
2. 按完整普通代理流程实现主界面选核、失败回滚、混合批测及分类结果、统计、导出、订阅偏好和异常恢复。它们属于同一版的完成条件。
3. 完成基础 TUN 切换以及 DNS/FakeIP/分流回归，使用户无需先关 TUN 再切核。
4. 前三步验收后形成首个完整版本，再新增 AnyTLS、Snell 和 Hysteria2 必要新选项。新增协议的导入、编辑、测试、导出和订阅更新同时完成，高级项默认折叠。
5. SSH、ShadowTLS、WireGuard 编辑、NaiveProxy、OpenVPN/OpenConnect、Tailscale 及保留旧连接的方案按实际需要独立安排。

首版以“旧配置直接使用 → 导入 → 连接 → 手动或自动选核 → 混合批测 → 更新订阅 → 重启后保留选择 → TUN 下重复操作”为验收主线。保持现有 Xray 兼容内核，后续协议共用同一套选核和测试机制。具体阶段、状态文案及场景清单见[复核后的开发计划](C:/Users/zqf/Documents/aaa/nekoray/docs/Dynamic_Core_Development_Plan.md)。

参考资料：[sing-box 1.14.0 发布说明](https://github.com/SagerNet/sing-box/releases/tag/v1.14.0)、[固定版本变更记录](https://github.com/SagerNet/sing-box/blob/v1.14.0/docs/changelog.md)、[出站协议文档](https://sing-box.sagernet.org/configuration/outbound/)、[V2Ray 传输](https://sing-box.sagernet.org/configuration/shared/v2ray-transport/)、[NaiveProxy](https://sing-box.sagernet.org/configuration/outbound/naive/)、[Selector](https://sing-box.sagernet.org/configuration/outbound/selector/)。
