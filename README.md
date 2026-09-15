# Nekoray 双内核便携版

项目仓库：[wanqiua/nekoray](https://github.com/wanqiua/nekoray)。本项目基于 [NekoRay 3.26](https://github.com/MatsuriDayo/nekoray) 二次开发，沿用 [GPL-3.0 许可证](LICENSE)。

当前提供 **Windows x64 便携版**，实测环境为 Windows 11。内置 **Matsuri Xray 1.8.4 兼容版**和 **sing-box 1.14.0**，保留 Xray 的 `rc4-md5` 与原有跳过证书验证（`allowInsecure`）设置。

## 下载与使用

当前版本：**[v3.26-dual-core.2](https://github.com/wanqiua/nekoray/releases/tag/v3.26-dual-core.2)**。

- [Windows x64 便携包](https://github.com/wanqiua/nekoray/releases/download/v3.26-dual-core.2/nekoray-3.26-dual-core.2-windows-x64.zip)
- [对应源码包](https://github.com/wanqiua/nekoray/releases/download/v3.26-dual-core.2/nekoray-3.26-dual-core.2-windows-x64-source.zip)
- [SHA-256 校验文件](https://github.com/wanqiua/nekoray/releases/download/v3.26-dual-core.2/nekoray-3.26-dual-core.2-windows-x64.sha256)

解压整个文件夹后运行 `nekoray.exe`。包内包含 Qt、TLS 和 VC++ 运行库，配置保存在程序目录的 `config` 文件夹。

升级时先退出旧版，备份并复制旧版 `config` 文件夹到新目录，无需重新导入节点和订阅。回退时使用原安装与备份；旧版不识别新增协议。详见[便携版使用说明](docs/Portable_Windows.md)。

本次沿用 `.2` 已验收的程序，整理 Windows 说明并发布便携包。每次版本更新使用独立标签，历史版本见[版本记录](docs/Versioning.md)。

## 主要功能

- 逐节点选择“自动 / Xray / sing-box”；两核都兼容时可手动切换，只有一核支持完整配置时自动选择它。
- 混合内核批量 URL 测试共用总并发预算，默认并发 10；结果记录实际内核及配置，避免历史、取消和“不适用”结果误判节点。
- 系统代理和 TUN 配合连接切换；预检失败时保持当前连接，启动失败时尝试恢复原配置。
- AnyTLS、Snell 4/6 和 Hysteria2 扩展选项，包含导入、编辑、分享和订阅更新。
- 订阅更新保留可唯一匹配节点的内核偏好和测试历史；解析失败时保留原分组。
- 主界面右键分组操作，链接导入使用选择按钮，分组类型使用单选按钮。

功能和兼容范围见[实现说明](docs/Dynamic_Core_Implementation.md)及[内核兼容说明](docs/Core_Upgrade.md)。

## 协议与配置

- SOCKS 4/4a/5、HTTP(S)
- Shadowsocks、VMess、VLESS、Trojan
- AnyTLS、Snell 4/6、TUIC、Hysteria、Hysteria2（sing-box）
- 自定义出站、完整配置和外置内核

订阅支持常见节点链接，以及已适配的 Clash/YAML、Shadowsocks、v2rayN 等格式。协议选项按实际内核能力检查，无法完整表达的配置会说明原因。

## 验证记录

Windows 11 的系统代理、TUN 切核、FakeIP 和故障恢复验收通过，2026-09-15 完成了拒绝不兼容 FakeIP 设置后保持连接的补充复验。旧配置升级、保存重启、原版回退启动及便携包 TLS 验收也已通过。

- [系统代理与 TUN 验收](tests/acceptance/system-host.md)
- [新协议验收及实测范围](tests/acceptance/new-protocols.md)
- [便携包与配置验收](tests/acceptance/package_acceptance.md)

## 获取源码与构建

```sh
git clone --recurse-submodules https://github.com/wanqiua/nekoray.git
cd nekoray
```

构建方法见 [Windows 构建教程](docs/Build_Windows.md)和[内核构建教程](docs/Build_Core.md)。启动选项见[运行参数](docs/RunFlags.md)。

## 致谢

- [MatsuriDayo/NekoRay](https://github.com/MatsuriDayo/nekoray)
- [MatsuriDayo/Xray-core](https://github.com/MatsuriDayo/Xray-core)、[XTLS/Xray-core](https://github.com/XTLS/Xray-core)
- [SagerNet/sing-box](https://github.com/SagerNet/sing-box)
- [Qv2ray](https://github.com/Qv2ray/Qv2ray)、[Qt](https://www.qt.io/)
- [protobuf](https://github.com/protocolbuffers/protobuf)、[yaml-cpp](https://github.com/jbeder/yaml-cpp)
- [ZXing](https://github.com/nu-book/zxing-cpp)、[QHotkey](https://github.com/Skycoder42/QHotkey)
