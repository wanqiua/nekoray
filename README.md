# NekoRay_BY_WanQiu

基于 [NekoRay](https://github.com/MatsuriDayo/nekoray) 的双内核 Windows 便携版，沿用 [GPL-3.0 许可证](LICENSE)。项目仓库：[wanqiua/nekoray](https://github.com/wanqiua/nekoray)。

当前版本：**[0.1](https://github.com/wanqiua/nekoray/releases/tag/v0.1)**。内置 **Matsuri Xray 1.8.4 兼容版**和 **sing-box 1.14.0**。

## 下载与使用

- [Windows x64 便携包](https://github.com/wanqiua/nekoray/releases/download/v0.1/NekoRay_BY_WanQiu-0.1-windows-x64.zip)
- [对应源码包](https://github.com/wanqiua/nekoray/releases/download/v0.1/NekoRay_BY_WanQiu-0.1-windows-x64-source.zip)
- [SHA-256 校验文件](https://github.com/wanqiua/nekoray/releases/download/v0.1/NekoRay_BY_WanQiu-0.1-windows-x64.sha256)

解压整个文件夹后运行 `NekoRay_BY_WanQiu.exe`。包内包含 Qt、TLS 和 VC++ 运行库，配置保存在程序目录的 `config` 文件夹。支持 Windows 10/11 x64，实际验收环境为 Windows 11。

升级时先退出旧版，备份并复制旧版 `config` 文件夹到新目录。旧配置目录及内核程序名继续兼容，无需重新导入节点和订阅。详见[便携版使用说明](docs/Portable_Windows.md)。

## 0.1 的修复

- “删除不可用配置”识别有效的 URL、TCP 和完整测试失败记录；优先采用 URL 结果，无可删项时说明原因。正在使用、过期、未测试及内核错误的配置保留。
- 修复 VLESS 订阅默认带有 `xudp` 时被错误判定为不支持 Xray 的问题，正确生成独立于 TCP 复用的 XUDP 配置。
- Hysteria2 支持 YAML 的 `fingerprint`、等价的 `ports` / `mport` / `server-ports`，以及链接中的 `pinSHA256`。证书指纹会实际校验，并保留到编辑、保存、分享和订阅更新。
- sing-box 对直接指向 `block` 的路由使用 `reject` 动作，正常广告拦截不再产生 `operation not permitted` 错误日志。
- 主界面内核选项保持一行，自动模式在右侧按钮显示目标内核；选择节点不会新增“下次连接”文字行。
- 使用独立产品名称和从 0.1 开始的版本号。订阅 UA 保留格式协商标识，并使用新版本号；切换默认内核不会改变 UA。

排查依据、UA 对比和修复范围见 [0.1 修复说明](docs/Release_0.1.md)。本次没有发现良心云在 NekoRay / NekoBox 的默认 UA 间改变节点传输参数；订阅更新失败和 XUDP 能力判断是另外两个原因。

## 功能与协议

逐节点选择“自动 / Xray / sing-box”；混合内核批量测速；系统代理与 TUN 切换、预检和失败恢复；订阅更新保留唯一匹配节点的偏好及测试历史。

支持 SOCKS 4/4a/5、HTTP(S)、Shadowsocks、VMess、VLESS、Trojan，以及使用 sing-box 的 AnyTLS、Snell 4/6、TUIC、Hysteria、Hysteria2。订阅支持常见节点链接、Clash/YAML、Shadowsocks 和 v2rayN 格式。

Xray 兼容版保留 `rc4-md5` 和原有跳过证书验证设置，不包含 XHTTP 等较新的 Xray 功能。Hysteria2 的证书 SHA-256 扩展由随包内核实现，不能把其配置直接交给未包含此扩展的官方 sing-box。

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
