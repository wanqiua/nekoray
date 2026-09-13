# NekoRay / PC 用 NekoBox

リポジトリ: [wanqiua/nekoray](https://github.com/wanqiua/nekoray) · [GPL-3.0 ライセンス](LICENSE)

QtベースのクロスプラットフォームGUIプロキシ設定マネージャ (バックエンド：v2ray / sing-box)

Windows / Linuxに対応。

## ダウンロード

### GitHub Releases (Portable ZIP)

[ダウンロード](https://github.com/wanqiua/nekoray/releases)

リリースがまだない場合は、[ビルド手順](docs/readme.md)に従ってビルドしてください。

[Portable 版の使い方](docs/Portable_Windows.md)

### 上流プロジェクトのパッケージ / Upstream packages

#### AUR

- [nekoray](https://aur.archlinux.org/packages/nekoray)
- [nekoray-git](https://aur.archlinux.org/packages/nekoray-git)

#### archlinuxcn

- [nekoray](https://github.com/archlinuxcn/repo/tree/master/archlinuxcn/nekoray)
- [nekoray-git](https://github.com/archlinuxcn/repo/tree/master/archlinuxcn/nekoray-git)

#### Scoop Extras

`scoop install nekoray`

## プロキシ

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

## サブスクリプション

- Raw: 広く使われているいくつかのフォーマット (Shadowsocks、Clash、v2rayN など)

## 動作パラメーター

[動作パラメーター](docs/RunFlags.md)

## Windows 実行

DLL が見つからない場合は、[Microsoft C++ ランタイムライブラリ](https://aka.ms/vs/17/release/vc_redist.x64.exe)をダウンロードしてインストールしてください

## Linux 実行

[Linux 実行チュートリアル](docs/Run_Linux.md)

## macOS

メンテナンス不足のため、macOS 版はダウンロードできなくなりました。

また、以下のリポジトリで非公式に更新版を入手することもできる。[非公式リリース](https://github.com/aaaamirabbas/nekoray-macos/releases)

以下のチュートリアルを参考に、自分でコンパイルすることもできます。FAQ については歴史的な Issue を参照し、修正については Pull Request を歓迎します。

## コンパイルチュートリアル

[技術ドキュメント](docs/readme.md)を参照して下さい

## クレジット

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
