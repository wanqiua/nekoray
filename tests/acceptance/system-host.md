# Windows 系统代理与 TUN 验收

阶段 1 验收记录，更新于 2026-09-15。Windows 11 Enterprise LTSC 主机的完整系统验收已通过，包含系统代理、TUN 转发、FakeIP 与故障恢复；9 月 15 日补充复验也已通过，确认拒绝不兼容 FakeIP 范围设置后原 TUN 继续转发。原生 UAC 拒绝及随后授权有单独实测证据。Windows 10、IPv6 FakeIP 连接未在本机验证。

验收对象是 `build/core-upgrade-check/gui-check/nekoray_gui_tests.exe` 及同目录两份内核。当前原安装位于 `deployment/packages/nekoray-3.26-dual-core-windows-x64-20260910/nekoray`。脚本会核对实际进程路径、PID 和创建时间，安装目录不匹配时拒绝运行。

系统代理测试使用回环 HTTP 代理节点。TUN 测试使用回环 SOCKS5、UDP 和 DNS 服务端，访问文档保留地址 `203.0.113.0/24`、`2001:db8::/32` 和测试域名。验收不加载真实用户节点。

## 执行方式

从仓库根目录执行下列命令，只进行只读预检和保存摘要：

```powershell
python tests/acceptance/system_host_acceptance.py `
  --gui-tests build/core-upgrade-check/gui-check/nekoray_gui_tests.exe `
  --existing-install deployment/packages/nekoray-3.26-dual-core-windows-x64-20260910/nekoray `
  --output-dir build/system-host-acceptance
```

追加 `--execute` 才运行真实系统测试，默认预算为 180 秒，包含 UAC 等待。需要单独验证拒绝授权时，设置 `NEKORAY_UAC_DENIAL_FIRST=1`；该模式要求首个 Windows 授权提示选择拒绝，第二个选择允许。变量未设置时直接验证允许授权后的数据转发。Windows 提示由操作人员处理，脚本不自动点击授权。

普通 CTest 不设置 `NEKORAY_SYSTEM_ACCEPTANCE=1`，这两个系统测试会跳过。守护脚本要求结构化 QTest 报告中 `systemProxyRuntimeAcceptance` 和 `tunSystemRuntimeAcceptance` 都实际通过，跳过、缺失或失败均不算成功。

## 暂停与恢复范围

外层脚本启动独立守护进程。守护进程保留原 GUI 及其安装目录内子进程的精确句柄，先暂停 GUI，再暂停内核；结束时逆序恢复。Windows `System32/conhost.exe` 仅确认身份后跳过。脚本不会按进程名全局停止程序，也不重新启动原安装或改写其配置。

原配置在全部目标进程暂停后计算文件哈希，在恢复前再次核对。代理设置备份使用当前用户的 DPAPI 加密；每次运行创建独立目录，其 ACL 仅允许当前用户和 SYSTEM。公开报告包含数量、哈希和检查结论，原代理地址、PAC 内容及节点配置不会写入公开摘要。

测试 GUI 在启用系统代理前，把本次合成回环代理地址写入审计文件。守护进程只在当前代理地址属于该审计记录、标志仍为 `DIRECT | PROXY`、旁路与 PAC 仍等于原快照时恢复设置。其他程序的不同设置会保留并使验收失败。WinINet 正常更新会增加 `Connections` 二进制记录中的计数；报告分别记录原始字节变化和去除该计数后的设置一致性。

超时、取消或外层脚本退出时，守护进程使用自身 `Popen` 保存的句柄结束本次测试 GUI，给予内核 owner watchdog 至多 18 秒清理。随后恢复原进程，并核对代理、路由、DNS 与网卡快照。无法核实存活内核身份时按清理未确认处理，不能当作进程已退出。正常退出和超时的完整宿主恢复均已由下述实测验证；外层意外退出分支仅有模拟覆盖，未单独中断主机运行测试。

真实测试启动后，守护进程在确认本轮临时内核全部退出、恢复原进程之前，调用 Windows DNSAPI 的 `DnsFlushResolverCache` 清空解析缓存，清除合成 DNS 应答可能留下的记录。这不会改写 DNS 服务器地址。`result.json` 的 `dns_cache_cleanup` 记录是否尝试以及是否成功；清理失败会使整体验收失败，但仍继续恢复原代理和进程。预检不会调用缓存清理。实际执行中若测试未启动或内核退出尚未确认，也不会清理缓存，并会记录未执行原因。缓存恢复顺序及失败路径已有纯模拟测试覆盖。

## 首轮结果

原始证据目录：`build/system-host-acceptance/run-0f658adfbfe247ddb3d4fa264d1db51b/`。

| 检查 | 结果与依据 |
|---|---|
| QTest 总结果 | 未通过，退出码 2；初始化和清理通过，两个系统测试失败，0 跳过。 |
| 系统代理 | 首版断言依赖 `QNetworkProxyFactory::systemProxyForQuery`，返回值未包含预期 HTTP 端口。该结果本身不能证明 Windows 设置写入失败。检测随后改为直接查询 WinINet，并用新的 `InternetOpen(PRECONFIG)` 会话请求合成端点。 |
| 首次拒绝 UAC | 本轮要求拒绝，但授权结果为允许，因此拒绝分支未覆盖并按失败记录。TUN 辅助进程实际开启，随后通过正常 Stop 关闭；后续数据转发断言未执行。 |
| 原进程恢复 | `original_processes_resumed=true`；暂停范围为 1 个 GUI、1 个内核。 |
| 原配置保持 | 112 个配置文件，`original_config_unchanged=true`。 |
| 系统代理恢复 | `semantic_state_restored=true`、`registry_restored=true`；GUI 正常清理已恢复设置，守护进程额外恢复连接数为 0。原始注册表字节因更新计数改变。 |
| 路由、DNS、网卡恢复 | 原有 37 条路由、10 条 DNS 记录、16 个网卡记录；三项快照均恢复，新增和减少数均为 0。 |
| 本次内核遗留 | `remaining_test_directory_core_count=0`。 |

详细证据为该目录中的 `result.json`、`qtest.txt` 和 `qtest.xml`。首轮只证明上述恢复路径，不能用于宣称 IPv4/IPv6、UDP、DNS、FakeIP、分流或 UAC 拒绝已经通过。

## 后续实测

证据目录：`build/system-host-acceptance/run-64588568260942f1bd6f8668b35ab7ec/`。QTest 退出码为 2，两个系统测试仍失败；下面只记录各自实际走到并通过的断言。

| 检查 | 结果与依据 |
|---|---|
| Windows 原生系统代理请求 | 三轮新的 WinINet 会话均得到合成端点 HTTP 204；最终总请求计数为 5，预期为 3，说明当前服务端计数混入后台请求。该用例整体仍失败，后续改为只统计本次测试的唯一标记。 |
| UAC 拒绝与随后授权 | 首次拒绝被原生代码正确识别，TUN 保持关闭且没有本次辅助进程 PID；随后授权成功并进入真实数据转发。日志含 `PASS: native UAC denial leaves TUN disabled and no owned helper PID`。 |
| 两核切换与 TUN | 按 Xray → sing-box → Xray 运行三轮；各轮 IPv4/IPv6 TCP、UDP 和 A/AAAA DNS 均通过，同一个 TUN 辅助进程保持运行。UDP 同时核对回复来源及合成 SOCKS 服务端实际目的地址。 |
| 目标内核 Start 失败 | 占用目标 HTTP 端口使 Start 真实失败，原会话、原内核偏好及 TUN 辅助进程恢复；随后 IPv4 TCP 和 UDP 实际转发通过。 |
| 本次代理内核崩溃 | 仅结束测试持有的代理内核进程；新一代内核恢复，TUN 辅助进程保持不变，随后 TCP 和 IPv6 UDP 转发通过。 |
| 阻断规则 | 保留地址 `203.0.113.34` 的 HTTP 失败，合成服务端请求数未增加，通过阻断断言。 |
| FakeIP | DNS 返回地址位于配置的假地址池；随后访问 FakeIP 的 HTTP 失败，尚未到达实际目的地址还原断言。主任务正在核对测试 CIDR 范围与域名还原后匹配的关系，当前不能宣称 FakeIP 转发通过。 |
| 原进程与配置 | 原 GUI 和内核恢复；112 个配置文件哈希保持不变。 |
| 系统状态与缓存 | 原代理语义、归一化注册表及 37 条路由、10 条 DNS 记录、16 个网卡记录全部恢复；本次内核遗留为 0。`dns_cache_cleanup.attempted=true`、`succeeded=true`。 |

## 本轮复核与本地测试

- TUN 生命周期已提取为生产与测试共用的状态转换代码，17 个故障注入用例通过，包括停止期间进程退出、RPC 确认丢失、回滚失败及取消后清理。日志位于 `build/tun-lifecycle-check/tun-lifecycle-results.log`。
- 合成 SOCKS 服务端已经记录实际 TCP/UDP 目的地址；FakeIP 断言要求目的地恢复为 `acceptance.example.test` 或其预期解析地址，并拒绝把原始 FakeIP 直接发给代理。共享回环 fixture 已验证 IPv4、IPv6、域名 UDP、DNS 和关联连接关闭，全局代理查询为 0；证据为 `build/system-fixture-check/fixture-results.log`。
- 系统代理部分写入失败后丢失恢复状态的问题已修复：写后读回、部分写入恢复、未确认清理的重试及保留其他程序修改均有覆盖。`build/dynamic-core-check/system-proxy-lease-results.log` 为 14/14 通过，包含初始化和清理。
- 退出流程在系统代理恢复尚未确认时保留当前连接，解除退出锁并允许重试；不会先停止代理内核再丢失恢复能力。
- Windows Xray 的通配 UDP 监听曾在 TUN 激活时错误绑定物理网卡，使回环 SOCKS 中继报 `wsasendto` 地址错误。包装层现对未指定源地址的回环 UDP 使用对应回环源地址，保留非回环接口绑定和显式配置。本地回归覆盖双回环收发、非回环套接字绑定读回、自定义拨号器及禁用自动接口选项；完整 Xray 模块回归通过。证据为 `build/system-host-review/protected-udp-before.log`、`protected-udp-after.log` 和 `xray-wrapper-regression.log`。
- 守护脚本已修正“读取内核身份失败就忽略”的判定。只有重新枚举确认进程消失，才可以视为退出；仍存在但无法核实时验收失败。新增缓存清理后，11 个纯模拟测试通过，覆盖成功、QTest 失败、启动后异常、清理失败仍恢复进程、未启动及预检不清缓存。证据为 `build/system-host-review/guardian-dns-mock-results.log`；这些测试不调用真实 DNS 清理、网络配置或进程暂停 API。

## 最终完整系统用例

证据目录：`build/system-host-acceptance/run-a4bbc8e82135488d831bd017e2063edf/`。QTest **4/4 通过、0 失败、0 跳过，26.568 秒**；守护报告 `passed=true`。

- 系统代理请求使用本次唯一 HTTP 标记，Xray → sing-box → Xray 三轮原生 WinINet 请求均得到 204，准确计数为 3；后台程序请求不再影响计数。
- TUN 三轮 IPv4/IPv6 TCP/UDP、A/AAAA DNS、阻断规则、目标 Start 失败回滚、拥有的代理内核崩溃恢复全部通过，TUN helper PID 不变。
- FakeIP 先验证 DNS 假地址池，再连接假地址；SOCKS 服务端实际接收 `acceptance.example.test:80`，没有收到原始 FakeIP。测试白名单包含验收进程，因为 FakeIP 还原为域名后，单独匹配假地址 CIDR 不代表允许该域名连接。
- 原进程恢复，112 个配置文件哈希不变；原系统代理语义及归一化注册表一致；37 条路由、10 条 DNS、16 个网卡记录均恢复；残留测试内核为 0，DNS 缓存清理成功。

此轮按已验证的 UAC 拒绝结果直接进行授权。拒绝分支的证据仍为上述 `run-64588568260942f1bd6f8668b35ab7ec/`，不把后续授权通过算作新增拒绝覆盖。

上一轮 `run-7a7f995d0b2d418bae6e62e9a5a05e6d/` 超过 210 秒验收时限。守护进程结束本次 GUI，等待内核 owner watchdog 清理，并恢复所有原状态；原配置和网络快照均通过核对。该轮缺少阶段日志，不能确定超时是未操作的 UAC 还是其他等待。最终轮补充阶段日志后正常完成。外层进程意外退出的分支有模拟覆盖，未再单独中断主机运行测试。

## FakeIP 范围保护的补充验收

后续预检增加了外置 TUN + FakeIP + 非空 CIDR + 空/AsIs/as_is 域名策略的组合保护，包含混合进程/CIDR 和黑名单场景。解析还原后的域名没有原始假地址匹配语义，不能默认认为假地址 CIDR 仍能约束它。组合不兼容时提示改用进程范围或调整策略，保留原设置和正在运行的 TUN。能力测试包含 17 个新增拒绝组合，整套 43 项通过。

更新后的主机用例会在一个已运行的 TUN 上请求不兼容组合，检查 helper PID、RPC 客户端、配置指纹、会话和设置不变，并继续验证 TCP/UDP/DNS，之后再切到兼容的进程范围验证 FakeIP。

`build/system-host-acceptance/run-1e0faa13bbb046a6bc69dacc918f3eb5/` 的系统代理部分通过，但 TUN 一直等待 Windows 授权，`helper_pid=0`，210 秒后由守护进程结束；**新 TUN 断言尚未执行，不能记为通过**。现场观察到对应 `consent.exe` 自提示阶段起保持存在，阶段日志也持续记录等待授权，区别于早先原因未确定的超时。

本轮超时后原代理和进程已恢复，112 个配置文件哈希未变；系统代理语义、归一化注册表和 37 条路由、10 条 DNS、16 个网卡快照全部恢复，残留测试内核 0，DNS 缓存清理成功。原生 UAC 窗口可能在调用方结束后仍需操作人员关闭，不能自动确认或把它视为新一轮授权。该轮没有覆盖的新断言已在下面的 9 月 15 日复验完成；此前完整转发和原生 UAC 拒绝的通过证据继续对应各自原始轮次。

## 2026-09-15 补充复验通过

证据目录：`build/system-host-acceptance/run-9501cb8769bb462bb9f6c351570469e1/`。QTest **4/4 通过、0 失败、0 跳过，13.011 秒**，包含初始化与清理；系统代理和 TUN 两个用例均实际执行。守护报告 `passed=true`，`failures=[]`，进程正常退出，未由守护进程强制结束。

- 新一轮 Windows 原生授权成功后，Xray → sing-box → Xray 的 IPv4/IPv6 TCP/UDP、A/AAAA DNS 全部通过，切核期间 TUN helper PID 不变。
- 在真实运行的 TUN 上请求不兼容 FakeIP/IP 范围组合，预检明确拒绝。helper PID、RPC 客户端、配置指纹和会话保持；设置草稿保持原值，未悄悄重写；拒绝后 TCP、UDP 和原 DNS 应答继续通过。
- 改用兼容进程范围后，FakeIP DNS 返回假地址池地址，通过 SOCKS 桥接访问时实际恢复为 `acceptance.example.test:80`。服务端未收到原始 FakeIP。
- 阻断规则、目标 Start 失败回滚、拥有的代理内核崩溃恢复和正常关闭均通过。日志包含 `PASS: real TUN ... rejected-FakeIP settings preserve forwarding ... normal cleanup`。
- 原 GUI 与内核已恢复，112 个配置文件哈希不变；系统代理语义及归一化注册表一致；37 条路由、10 条 DNS、16 个网卡快照恢复，新增和减少记录均为 0。残留测试内核为 0，DNS 缓存清理成功；结束后没有残留 UAC 提示。

本次第一轮 `run-befcd2fc27dc41de89bffe48def63a6e` 中，用户告知误点“否”。通过该轮专属 `cancel` 文件通知守护进程结束等待，确认原进程、配置和全部网络快照恢复后才重跑。该轮按取消记录，不算 TUN 通过，也不重复计为原生拒绝验收。

复验复用最终构建，未改产品代码或重新打包。GUI 测试程序 SHA-256 为 `effe97e01c68e8541744d90e3f7d16e57a0bfeb048f23005e7d3b5eb81a9422f`；Xray 为 `dd66a1fc05c1c4ad628d9c628c856ad8c74ac87b3efa52acaedb16027adb6882`，sing-box 为 `233ea9386b2f8a13f62b333845191e1cf738f32295fbc7f8644c70abb5181771`。这些哈希在运行前已核对，与 9 月 13 日最终构建一致。本页已有的 Windows 10、IPv6 FakeIP 与外层进程意外退出实测范围保持不变。
