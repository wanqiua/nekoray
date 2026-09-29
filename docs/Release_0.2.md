# NekoRay_BY_WanQiu 0.2

本版将 Xray 升级到 **wanqiua/Xray-core 26.3.27-matsuri.1**，保留旧协议兼容并接入新版传输和证书指纹。sing-box 保持 **1.14.0**。

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
