候选包与旧配置验收工具，更新于 2026-09-15。此目录只包含开发验收入口，不应打入便携二进制包。工具支持历史 `nekoray` 目录及 0.1 的 `NekoRay_BY_WanQiu` 目录与主程序名；以下历史记录仍绑定各自日期和哈希。

**已准备的检查**

- ZIP 文件路径、重复项、文件集合、长度和 SHA-256；核对两个内核清单、Matsuri Xray 1.8.4 固定提交及 sing-box 1.14.0。
- 直接从两个 Windows amd64 EXE 的 PE 数据段读取 Go 内嵌元数据，逐字核对 `licenses/*-build-info.txt`，包含工具链、模块校验和、替代来源和构建设置。此步骤只读文件，不运行 Go 或内核。
- 核对 `licenses/go-modules.json` 与实际链接模块的完整集合、版本、替代来源及所属内核，校验 notices 目录标识、文件数量和非空内容；sing-box 必须实际链接 AnyTLS、Snell 和 QUIC 模块。
- 同目录源码包的哈希、CRC、必要源码及工作区快照标识；覆盖新协议 Bean、导入和编辑器、能力声明、HY2 配置、共享 RPC 源码、测试及 notices 生成器。拒绝运行目录、用户配置、日志、DPAPI 快照和 `__pycache__`。
- 分别生成 Xray、sing-box 来源的合成 3.26 配置。包括不连续的节点和分组 ID、分组与节点顺序、重名节点、订阅信息、流量、路由、测试设置、`rc4-md5` 和跳过证书校验字段。
- 每组配置经历首次升级保存、退出后重新启动、保存默认与逐节点偏好、再次退出和启动。逐项核对旧字段和偏好，旧延迟只进入历史且不重复迁移；另一套来源设置文件保持字节不变。
- 升级前备份和回退副本保留在不同目录，核对完整原始字节。回退使用升级前配置，不能让旧程序直接处理之后新增的不认识的协议节点。
- `package_native_acceptance.py` 在新提取副本启动清单指定的实际 GUI（0.1 为 `NekoRay_BY_WanQiu.exe`），分别打开两套来源配置，检查合成分组、DLL 来源和正常退出；另复制原版便携程序并用升级前合成备份启动。
- 包内 TLS 正例必须验证 localhost 证书并返回 HTTP 204；错误主机名证书必须握手失败。临时证书仅供测试连接信任，不修改证书存储。

测试入口复用生产 `main.cpp` 的启动流程，并通过主窗口正常退出。它不连接节点、不更新订阅，也不启用系统代理或 TUN。它需要生成的配置标记和独立提取目录的标记，不能接收普通用户配置目录。Windows 进程在启动前加入独立 Job；超时只清理该 Job，正常退出后仍残留子进程会判为失败。清理会等待进程句柄结束和 Job 中活动进程归零，避免日志尚未释放就开始检查下一阶段。

夹具遵循 3.26 的实际持久化格式：`JsonStore::ToJson` 省略空字符串，入站认证使用 `user`/`pass`。非空字段仍严格比较，不能把丢失订阅地址、密码等内容归为“空值规范化”。迁移后清空的旧报告允许按原序列化规则省略。

**当前阶段可运行的准备检查**

Python 工具需要 Python 3.11+，无额外 Python 依赖。`prepare`、`static` 及 Go 元数据测试不需要 Go 或 PowerShell；PowerShell 脚本和原生 TLS 验收需要 PowerShell 7。

```powershell
python -X utf8 -m unittest discover -s tests/acceptance -p 'package_*test.py' -v
./tests/acceptance/package_source_inventory_test.ps1
python -X utf8 tests/acceptance/package_acceptance.py prepare
```

`prepare` 只在 `build/package-acceptance/<随机目录>` 生成合成配置和备份。不会运行 GUI、内核或构建包。打包脚本新增 `-ValidateInputsOnly`，使用正式打包的同一组输入参数即可检查资源、清单、内核哈希和源码清单，不创建 staging 或 ZIP。

当前共 80 项 Python 测试：`package_native_acceptance_test.py` 的 25 项全部模拟进程和 GUI 观察，不启动 PowerShell、GUI、内核或网络连接；`package_go_build_info_test.py` 的 21 项使用合成 PE，覆盖区段边界、长度溢出、格式、替代模块和通知清单；`package_acceptance_test.py` 的 34 项包含 3 项真实 Windows Python 子进程的 Job 所有权检查，其余为文件和夹具检查，包含新产品名与目录验证。归档遗漏源码/通知或携带旧元数据时，即使归档清单的哈希都正确，也必须在提取前拒绝。

只豁免精确的本地依赖 `grpc_server@v1.0.0 => ../../grpc_server@(devel)`，其实现必须进入源码包。固定的 `libneko` 快照没有独立 LICENSE，记录允许 `notice_files=0`，但必须包含原 README、`SOURCE-NOTICE.txt` 及明确的来源说明；不能把它误报为许可证丢失，也不能用同一豁免放行其他模块。notice 目录检查只验证发布资料与二进制的一致性，不声称核验上游源码的真实性。

源码收集先遵守 Git 的 ignore，再作明确排除。根目录的 `deployment`、`config`，任意层级的 `build*`（包括 `build64`、`build_debug`）、`cmake-build-*` 和 `__pycache__` 目录，以及运行日志、转储、DPAPI、编译产物均不纳入源码包。固定子模块的生成目录在遍历前就被排除；符号链接和 junction 不作为源码复制。

**进入候选包验收阶段后的命令**

先使用修复了 MSVC 头文件依赖的构建环境配置，再按需构建两个验收程序。它们设为 `EXCLUDE_FROM_ALL`，不会跟随常规构建或 CTest 自动运行。

```powershell
cmake --build build/core-upgrade-check/gui-check --target nekoray_package_probe nekoray_package_smoke --parallel 8

python -X utf8 tests/acceptance/package_acceptance.py static `
  --archive 'deployment/packages/<候选包名称>.zip'

python -X utf8 tests/acceptance/package_acceptance.py lifecycle `
  --archive 'deployment/packages/<候选包名称>.zip' `
  --probe build/core-upgrade-check/gui-check/nekoray_package_probe.exe

python -X utf8 tests/acceptance/package_native_acceptance.py `
  --archive 'deployment/packages/NekoRay_BY_WanQiu-0.1-windows-x64.zip' `
  --smoke 'build/core-upgrade-check/gui-check/nekoray_package_smoke.exe' `
  --legacy-source 'build/core-upgrade-check/reference/nekoray' `
  --powershell 'C:/Program Files/PowerShell/7/pwsh.exe'
```

`static` 先核对整个 ZIP 与配套源码包，再提取到全新目录。`lifecycle` 同样先验证原始包，然后才向测试副本加入验收程序和标记。`native` 使用实际 GUI 本体，并仅把测试副本的 `start_minimal` 改为 `false` 以观察窗口。所有结果、合成配置及日志留在独立运行目录，重复执行会生成新目录；指定 `--work` 时要求该目录尚不存在。

原生入口也支持 `--verified-payload '<static-work>/candidate/nekoray' --static-report '<static-work>/static-verification.json'` 代替 `--archive`。报告必须由当前静态验证器生成，包含源码包、Go build-info 和模块 notices 已验证标记、对应提取路径及 `manifest_sha256`。入口重新核对文件集合、大小和哈希，再复制到新目录；缺少这些标记的旧报告需重新生成。已经加入探针或 smoke 的运行副本不能冒充未修改的静态提取副本。

`acceptance.json` 汇总每个检查点；归档验证或探针复制失败也会记录失败报告，每次 GUI 保存和退出还写入对应的合成配置报告。对比错误只指出变化字段，不把节点凭据打印到控制台。`legacy_binary_launch` 单独标记旧 GUI 是否已经运行，不把“备份字节恢复成功”冒充旧程序启动成功。

原生报告为 `build/package-native-acceptance/<随机目录>/native-acceptance.json`。每个窗口另有 `*-result.json`、`*-ui.json` 和进程日志，失败也保留已经完成的窗口与模块观察。启动首个 GUI 前先检查 smoke、PowerShell、两个 GUI 及必要 DLL 是否完整。

GUI 进程使用保留的进程句柄、创建时间、PID、可执行文件路径和 HWND 核验身份。控制器只向该窗口内已观察到的“程序”按钮发送普通窗口点击消息，再通过同 PID 窗口的 UI Automation “退出”动作关闭；不发送全局鼠标输入、不通过进程名批量关闭。这样避免 Qt 的模态菜单阻塞同步菜单打开调用。正常退出必须由进程自身结束所有子进程；超时只由该进程树自己的 Job 收尾。

候选必须正常退出。旧版若只能验证启动，会明确报告 `result/scope=startup-only`、`normal_exit=false`、`legacy_normal_exit_verified=false`，并说明旧版保存/退出未验证；不能把 Job 强制清理算成正常退出。R1 和最终包验收中的旧版均完成了正常退出，没有用这项降级。

旧版来源只复制运行文件，遇到 `config`、`profiles`、`groups`、路由和日志目录就跳过，不遍历或读取其中的配置；使用另行生成的升级前备份副本。原版 3.26 参考包没有捆绑 VC++ CRT，旧版观察允许其从 System32 加载，并在 `runtime_modules[].origin=system-crt` 中记录路径与哈希；候选 Qt、插件、OpenSSL 和 CRT 仍必须来自候选副本。

`nekoray_package_smoke.exe` 是原有 Qt/TLS smoke 的可追踪源码版本。原生入口只在已验证的候选副本中加入该程序。`package_tls_fixture.ps1` 通过 PowerShell 7 的 .NET API 生成两套短期自签名证书，分别用于 localhost 和错误主机名。smoke 检查 HTTPS 204、证书验证，以及 Qt、Windows 平台插件、OpenSSL 和 VC++ 模块来源；负例必须返回握手失败。证书和私钥保留在独立测试目录，不导入系统或用户证书存储。

**2026-09-13 的 R1 实测证据**

本次候选 SHA-256 为 `7bdb0454b17e3fc22e8eecf3cf609bf9becd98b1ebac3e26a8b0059405151fd8`。下列结果只对应此包，后续修改重新打包后应对最终产物再验证。

- 生命周期：`build/package-acceptance/f32decae8f564ae5b30b82c9a8f38c56/acceptance.json`，10 个检查点全部通过。总日志为 `build/system-host-review/r1-package-lifecycle-v2.log`。
- 原生 GUI、旧版 GUI 与 TLS：`build/package-native-acceptance/e51048963d644e27a32f246421aa021c/native-acceptance.json`，四次 GUI 均显示两组合成分组并正常退出，`legacy_normal_exit_verified=true`；TLS 正反例均通过。总日志为 `build/system-host-review/r1-package-native-v5.log`。
- 夹具和静态/Job 测试 25 项、纯模拟原生编排测试 25 项通过。早期失败来自夹具的空字符串/认证字段格式及 UIA 菜单控制器，修正后再执行上述实测通过。

R1 之后新增 Go 元数据门禁：当前 `deployment/windows64` 的两个 EXE 与 `build/core-upgrade-check/package-notices-release-20260913` 已通过静态核对，分别有 37、76 个外部模块，按来源去重后 112 项。记录为 `build/system-host-review/package-go-metadata-static.json`；77 项单元测试日志为 `build/system-host-review/package-acceptance-tests-metadata.log`。这项记录验证打包输入，不代替最终 ZIP 的静态及原生验收。

**2026-09-13 的最终包实测证据**

最终包为 `deployment/packages/nekoray-3.26-dual-core-windows-x64-20260913.zip`，SHA-256 为 `daa1cda05940e736fd713ddfa91df461662b151c792c3d51c7fcb6ce67e80e6f`。配套 `nekoray-3.26-dual-core-windows-x64-20260913-source.zip` 的 SHA-256 为 `a4791de374ae19c64feb03116f44adf061fb63c4a6990e64e2445e48eb9fffe8`。两者均与 `.sha256` 文件一致。实际启动的候选 GUI SHA-256 为 `225c0d976710322704c08af42fce6a358eb981c891d7cf0c2297e52d53f72c9b`。

- 静态检查：`build/package-acceptance/dac4e2083f154e05a043a377300d10b8/static-verification.json`。349 个清单文件的路径、大小和哈希、源码包及 112 个 Go 模块均通过；两个内核的实际嵌入元数据与打包 notices 一致。总日志为 `build/system-host-review/final-package-static.log`。
- 配置生命周期：`build/package-acceptance/856283da49b64dcd898996a99e5d2a5b/acceptance.json`。Xray/sing-box 两套来源配置各完成升级、重启、偏好保存、再次重启和原始备份核对，共 10/10 个检查点通过。订阅、分组和节点次序、`rc4-md5`、跳过证书校验等合成原字段保留。总日志为 `build/system-host-review/final-package-lifecycle.log`。
- 原生 GUI 与 TLS：`build/package-native-acceptance/76bfe71d0ea54d6cbdc81ec6ec85c6b6/native-acceptance.json`。候选与旧版各启动两套来源配置，四次均显示合成分组，并通过各自窗口的退出菜单正常退出；`legacy_normal_exit_verified=true`。候选运行库全部来自测试副本，旧版 CRT 的 System32 来源按原边界单独记录。可信 localhost 证书返回 HTTP 204，错误主机名证书握手被拒绝，2/2 通过。总日志为 `build/system-host-review/final-package-native.log`。

以上记录绑定这次最终产物的哈希。包验收只使用独立运行副本和合成配置，`real_user_configuration_accessed=false`、`system_modes_enabled=false`，没有导入系统证书，也没有操作其他进程的授权提示。最终包测试结束后仅向本工作区文档补入证据，未重新生成或修改已经验收的 ZIP。

**本入口的验收范围**

这里的配置与便携程序检查不替代真实 TUN、系统代理、UAC 或真实服务端互通验收，这些由对应的专门报告记录。最后一项 TUN 补充验收已于 2026-09-15 通过，见 [系统验收记录](system-host.md) 的 `run-9501cb8769bb462bb9f6c351570469e1`；该次复验未修改已经验收的 ZIP，也不重复计入这里的包测试数量。
