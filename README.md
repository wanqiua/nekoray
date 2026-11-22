# NekoRay_BY_WanQiu

基于 [NekoRay](https://github.com/MatsuriDayo/nekoray) 的双内核 Windows 便携版，沿用 [GPL-3.0 许可证](LICENSE)。项目仓库：[wanqiua/nekoray](https://github.com/wanqiua/nekoray)。

当前版本：**[0.35](https://github.com/wanqiua/nekoray/releases/tag/v0.35)**。内置 **[wanqiua/Xray-core](https://github.com/wanqiua/Xray-core) 26.3.27-matsuri.1** 和 **sing-box 1.14.0**。

2026-11-22 修订：版本更新为 **0.35**，修复 sing-box 连接列表，并改用面板右上角的简约面板开关与轻量标签栏。

## 下载与使用

- [已发布稳定版及下载附件](https://github.com/wanqiua/nekoray/releases/latest)
- [Windows x64 便携包](https://github.com/wanqiua/nekoray/releases/download/v0.35/NekoRay_BY_WanQiu-0.35-windows-x64.zip)
- [对应源码包](https://github.com/wanqiua/nekoray/releases/download/v0.35/NekoRay_BY_WanQiu-0.35-windows-x64-source.zip)
- [SHA-256 校验文件](https://github.com/wanqiua/nekoray/releases/download/v0.35/NekoRay_BY_WanQiu-0.35-windows-x64.sha256)

解压整个文件夹后运行 `NekoRay_BY_WanQiu.exe`。包内包含 Qt、TLS 和 VC++ 运行库，配置保存在程序目录的 `config` 文件夹。支持 Windows 10/11 x64，实际验收环境为 Windows 11。

点击“更新”可下载仓库最新稳定版；退出后自动备份程序目录、覆盖程序文件并重启，`config` 保持原样。手动升级也可解压新包覆盖旧目录。详见[便携版使用说明](docs/Portable_Windows.md)。

## 0.35 的变化

- 接通 sing-box 原生路由跟踪接口，显示 TCP / UDP 活动连接及最近 512 条关闭记录，不需要开放额外的 HTTP 控制端口。
- 两个内核均需在“首选项 > 基本设置”中启用连接统计并重新连接节点。
- 下方面板右上角使用 20px 面板开关图标，透明无边框，悬停时显示浅灰底；日志 / 连接标签统一为轻量样式。收起后保留标签栏和展开入口，展开时恢复原高度。
- 程序版本、内核产品标识和便携包名称同步为 0.35；已有 0.3 发行包保持原样。
- 详细变更见 [0.35 发行说明](docs/Release_0.35.md)。

## 0.3 的变化

- 移除推广按钮；日志和连接面板可从标签栏右侧收起并恢复原高度。
- 连接列表显示未启用统计或当前没有连接记录的原因。
- Windows x64 自动更新只使用本仓库最新稳定发行包，更新前创建同级时间戳备份并保留 `config`。
- 详细变更见 [0.3 发行说明](docs/Release_0.3.md)。

## 0.2 的变化

- Xray 升级到 wanqiua 的 26.3.27-matsuri.1，保留 RC4-MD5、内置 SIP003 插件和旧传输兼容。
- HTTPUpgrade 接入界面；XHTTP 可通过自定义配置使用。
- 证书指纹改用新版原生校验，支持服务器发送证书链；适配新版 Windows UDP 网卡绑定与连接管理。
- 详细变更见 [0.2 发行说明](docs/Release_0.2.md)。

## 0.11 的修复

- Hysteria2 自签名证书可直接按指纹验证；修复指纹与不发送 SNI 的组合。
- 修复 insecure 别名和布尔值解析，新增 VLESS / Trojan / VMess 的证书指纹导入、编辑、分享和 Xray 校验。
- 详细变更见 [0.11 发行说明](docs/Release_0.11.md)。

## 0.1 的修复

- “删除不可用配置”识别有效的 URL、TCP 和完整测试失败记录；优先采用 URL 结果，无可删项时说明原因。正在使用、过期、未测试及内核错误的配置保留。
- 修复 VLESS 订阅默认带有 `xudp` 时被错误判定为不支持 Xray 的问题，正确生成独立于 TCP 复用的 XUDP 配置。
- Hysteria2 支持 YAML 的 `fingerprint`、等价的 `ports` / `mport` / `server-ports`，以及链接中的 `pinSHA256`。证书指纹会实际校验，并保留到编辑、保存、分享和订阅更新。
- sing-box 对直接指向 `block` 的路由使用 `reject` 动作，正常广告拦截不再产生 `operation not permitted` 错误日志。
- 主界面内核选项保持一行，自动模式在右侧按钮显示目标内核；选择节点不会新增“下次连接”文字行。
- 使用独立产品名称和从 0.1 开始的版本号。订阅 UA 保留格式协商标识，并使用新版本号；切换默认内核不会改变 UA。

排查依据、UA 对比和修复范围见 [0.1 修复说明](docs/Release_0.1.md)。本次没有发现良心云在 NekoRay / NekoBox 的默认 UA 间改变节点传输参数；订阅更新失败和 XUDP 能力判断是另外两个原因。

## 功能与协议

0.11 新增 VLESS / Trojan / VMess 的 `pcs` 证书 SHA-256 支持。链接中的 `allowInsecure`、`insecure`、`allow_insecure` 按此优先级解析，`1/true` 开启、`0/false` 关闭。Clash 的十六进制 `fingerprint` 同样导入；指纹保留到编辑、保存和分享，包括旧版 VMess JSON 分享格式。

这些协议填写指纹后只按指纹校验，自动模式选择 Xray；sing-box 不会静默丢弃指纹。当前源码使用 Xray 原生 `pinnedPeerCertSha256`，服务器发送多张证书时也可以验证叶证书指纹。REALITY 使用自己的认证机制，不使用 `pcs`。Xray 支持 VLESS + REALITY，但不支持 WebSocket + REALITY，这种组合会选择 sing-box。

审查依据、兼容边界和测试范围见 [TLS 证书指纹修复说明](docs/TLS_Certificate_Pins.md)。

逐节点选择“自动 / Xray / sing-box”；混合内核批量测速；系统代理与 TUN 切换、预检和失败恢复；订阅更新保留唯一匹配节点的偏好及测试历史。

支持 SOCKS 4/4a/5、HTTP(S)、Shadowsocks、VMess、VLESS、Trojan，以及使用 sing-box 的 AnyTLS、Snell 4/6、TUIC、Hysteria、Hysteria2。订阅支持常见节点链接、Clash/YAML、Shadowsocks 和 v2rayN 格式。

Xray 兼容版保留 `rc4-md5`、内置 SIP003 插件、旧 HTTP/2 / QUIC / mKCP 和原有跳过证书验证设置。HTTPUpgrade 可在界面配置；XHTTP 由内核支持，目前需使用 Xray 自定义配置。Hysteria2 的证书 SHA-256 扩展由随包内核实现，不能把其配置直接交给未包含此扩展的官方 sing-box。填写证书指纹后按指纹验证服务端证书（与 Clash Meta 一致），自签名证书无需开启跳过证书验证；指纹不符时拒绝连接。

## 源码、构建与验证

```sh
git clone --recurse-submodules https://github.com/wanqiua/nekoray.git
cd nekoray
```

- [Windows 构建](docs/Build_Windows.md) · [内核构建](docs/Build_Core.md)
- [实现说明](docs/Dynamic_Core_Implementation.md) · [内核兼容说明](docs/Core_Upgrade.md)
- [版本记录](docs/Versioning.md) · [运行参数](docs/RunFlags.md)
- [系统代理与 TUN 验收](tests/acceptance/system-host.md) · [协议验收](tests/acceptance/new-protocols.md) · [便携包验收](tests/acceptance/package_acceptance.md)

GUI 的上游代码基线是 NekoRay 3.26；它只用于说明来源，不再作为本分支的产品版本号。历史验收记录保留原日期和产物哈希，0.1 的发行附件提供本次产物的验证结果。

感谢 [MatsuriDayo/NekoRay](https://github.com/MatsuriDayo/nekoray)、[MatsuriDayo/Xray-core](https://github.com/MatsuriDayo/Xray-core)、[XTLS/Xray-core](https://github.com/XTLS/Xray-core)、[SagerNet/sing-box](https://github.com/SagerNet/sing-box)、Qv2ray、Qt、protobuf、yaml-cpp、ZXing 和 QHotkey。
