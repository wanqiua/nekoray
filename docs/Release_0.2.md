# NekoRay_BY_WanQiu 0.2

本版将 Xray 升级到 **wanqiua/Xray-core 26.3.27-matsuri.1**，保留旧协议兼容并接入新版传输和证书指纹。sing-box 保持 **1.14.0**。

## 0.2 fix1：URL 批测与 REALITY 修复

- URL 批测的超时、EOF、连接重置等临时失败会在首轮全部完成后串行复测一次，仍使用同一内核和原超时。首轮错误保留在历史中，复测结束前不会据此新增“不可用”删除候选；配置、证书、HTTP 状态错误不重试。
- 取消会停止排队及正在执行的复测；节点、测试设置或内核进程改变后不执行旧配置的复测。完整测速和单节点测试保持原有行为。
- Xray REALITY 的空指纹和 `chrome` 自动预设生成 `hellochrome_120`，`firefox` 自动预设生成 `hellofirefox_120`，避免新版自动指纹与部分已有 REALITY 服务端握手不兼容。明确指定的版本指纹、普通 TLS 和 sing-box 保持原值；编辑器提示与导出配置可查看实际行为。节点存储与分享参数不被改写。
- GitHub 修复包继续使用 `NekoRay_BY_WanQiu-0.2-windows-x64.zip`，在原 `v0.2` 发布中替换旧附件；先前本地验证包的 fix1 是修订标识，不是新版本号。GUI 版本仍为 0.2，两个内核与初版 0.2 相同；本次修复在 GUI 调度和配置生成层完成。

## 升级与修复

- Xray 固定到 `codex/matsuri-compat` 分支提交 `e9639bd461dcff1c91e26a71a8752fa42d51268d`，基于官方 v26.3.27。保留 RC4-MD5、旧 Shadowsocks 加密、内置 SIP003 插件、旧 HTTP/2 / QUIC / mKCP 和显式 `allowInsecure`。
- HTTPUpgrade 支持界面的 Host / Path 配置；内核支持 XHTTP，目前通过 Xray 自定义配置使用。
- VLESS / Trojan / VMess 的证书 SHA-256 使用原生 `pinnedPeerCertSha256`，服务器发送多张证书时也能验证叶证书指纹；无需强制开启跳过证书验证。旧自定义配置的整链指纹继续保持原有语义，错误指纹仍拒绝连接。
- 出站顶层旧 `domainStrategy` 在加载时转换为 `targetStrategy`；新旧字段冲突时报错，路由、freedom 和 sockopt 中的同名字段保持不变。
- 适配新版 Xray 的连接管理 API 和 Windows UDP 拨号方式，保留连接记录、关闭取消及 IPv4/IPv6 网卡绑定。
- GUI、内核版本标识、构建清单与打包来源校验同步更新。

## 下载与升级

下载附件 `NekoRay_BY_WanQiu-0.2-windows-x64.zip`，完整解压后运行 `NekoRay_BY_WanQiu.exe`。支持 Windows 10/11 x64，本次验证环境为 Windows 11。

退出旧程序，备份并复制旧版 `config` 文件夹到新程序目录。GUI 与随包内核需要配套更新；不要把独立的 `xray.exe` 改名替换 `nekoray_core.exe`。

## 验证与范围

Xray 适配层回归涵盖 RC4-MD5 TCP/UDP、TLS / Chrome uTLS、64 个新旧证书指纹连接用例、XUDP、HTTPUpgrade / XHTTP、连接生命周期和 Windows 网卡绑定。另验证双内核 RPC、单测与混合批测、GUI 配置生成和便携包的配置保存与重启。

本地测试使用合成节点与回环服务，本次未重新接管真实系统 TUN、连接个人订阅节点或进行 Linux 原生验收。XHTTP 尚无完整编辑表单；WebSocket + REALITY 仍使用 sing-box。

发行附件包含便携包、对应源码、SHA-256 和 `NekoRay_BY_WanQiu-0.2-verification.md` 验证记录。构建工具链为 Go 1.26.4，具体来源见包内 `core-versions.json` 和依赖许可证目录。
