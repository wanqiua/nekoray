# NekoRay / NekoBox For PC

项目仓库：[wanqiua/nekoray](https://github.com/wanqiua/nekoray)。本项目基于 NekoRay 二次开发，沿用 [GPL-3.0 许可证](LICENSE)。

本分支基于 **3.26** 保留双内核，使用 **Matsuri Xray v1.8.4 兼容版 / sing-box v1.14.0**。Xray 保留 `rc4-md5` 和 `allowInsecure` 支持。参见 [兼容说明](docs/Core_Upgrade.md) 和 [内核构建教程](docs/Build_Core.md)。

批量 URL 测试的并发与结果回写、分组右键菜单和链接导入窗口已完成修改。本地 Windows x64 便携包及对应源码包输出到 `deployment/packages/`，使用方法见[便携版说明](docs/Portable_Windows.md)，自行构建见 [Windows 构建教程](docs/Build_Windows.md)。

Qt based cross-platform GUI proxy configuration manager (backend: v2ray / sing-box)

Support Windows / Linux out of the box now.

基于 Qt 的跨平台代理配置管理器 (后端 v2ray / sing-box)

目前支持 Windows / Linux 开箱即用

## 获取二开源码

```sh
git clone --recurse-submodules https://github.com/wanqiua/nekoray.git
cd nekoray
```

GUI 和内核构建方法见 [Windows 构建教程](docs/Build_Windows.md) 与 [内核构建教程](docs/Build_Core.md)。

## Readme Translations

中文 / English / [fa_IR](README_fa.md) / [日本語](README_ja.md)

## 下载 / Download

### GitHub Releases (Portable ZIP)

[下载 / Download](https://github.com/wanqiua/nekoray/releases)

如果 Releases 中暂无发布版本，请参照[构建教程](docs/readme.md)自行编译。

[便携版使用说明](docs/Portable_Windows.md)

### 上游项目的第三方包 / Upstream packages

#### AUR

- [nekoray](https://aur.archlinux.org/packages/nekoray)
- [nekoray-git](https://aur.archlinux.org/packages/nekoray-git)

#### archlinuxcn

- [nekoray](https://github.com/archlinuxcn/repo/tree/master/archlinuxcn/nekoray)
- [nekoray-git](https://github.com/archlinuxcn/repo/tree/master/archlinuxcn/nekoray-git)

#### Scoop Extras

`scoop install nekoray`

## 代理 / Proxy

- SOCKS (4/4a/5)
- HTTP(S)
- Shadowsocks
- VMess
- VLESS
- Trojan
- TUIC ( sing-box )
- NaïveProxy ( Custom Core )
- Hysteria ( Custom Core or sing-box )
- Hysteria2 ( Custom Core or sing-box )
- Custom Outbound
- Custom Config
- Custom Core

## 订阅 / Subscription

- Raw: some widely used formats (like Shadowsocks, Clash and v2rayN)
- 原始格式: 一些广泛使用的格式 (如 Shadowsocks、Clash 和 v2rayN)

## 运行参数

[运行参数](docs/RunFlags.md)

## Windows 运行

若提示 DLL 缺失，无法运行，请下载 安装 [微软 C++ 运行库](https://aka.ms/vs/17/release/vc_redist.x64.exe)

## Linux 运行

[Linux 运行教程](docs/Run_Linux.md)

## macOS

由于缺乏维护，不再提供 macOS 版本下载。

您还可以在下面的存储库中非正式地获取更新版本。[非官方发布 / Unofficial releases](https://github.com/aaaamirabbas/nekoray-macos/releases)

仍可以参照下方教程自行编译，常见问题请参考历史 Issue，欢迎 Pull Request 修复。

## 编译教程 / Compile Tutorial

请看 [技术文档 / Technical documentation](docs/readme.md)

## Credits

Core:

- [v2fly/v2ray-core](https://github.com/v2fly/v2ray-core) ( < 3.10 )
- [MatsuriDayo/Matsuri](https://github.com/MatsuriDayo/Matsuri) ( < 3.10 )
- [MatsuriDayo/v2ray-core](https://github.com/MatsuriDayo/v2ray-core) ( < 3.10 )
- [XTLS/Xray-core](https://github.com/XTLS/Xray-core) ( >= 3.10 )
- [MatsuriDayo/Xray-core](https://github.com/MatsuriDayo/Xray-core) ( >= 3.10 )
- [SagerNet/sing-box](https://github.com/SagerNet/sing-box)
- [Matsuridayo/sing-box-extra](https://github.com/MatsuriDayo/sing-box-extra)

Gui:

- [Qv2ray](https://github.com/Qv2ray/Qv2ray)
- [Qt](https://www.qt.io/)
- [protobuf](https://github.com/protocolbuffers/protobuf)
- [yaml-cpp](https://github.com/jbeder/yaml-cpp)
- [zxing-cpp](https://github.com/nu-book/zxing-cpp)
- [QHotkey](https://github.com/Skycoder42/QHotkey)
- [AppImageKit](https://github.com/AppImage/AppImageKit)
