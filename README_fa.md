# NekoRay

مخزن پروژه: [wanqiua/nekoray](https://github.com/wanqiua/nekoray) · [مجوز GPL-3.0](LICENSE)

مدیریت پیکربندی پروکسی با رابط کاربری مبتنی بر QT , کراس پلتفرم . (بک اند : v2ray / sing-box )

اکنون از Windows / Linux / macOS amd64 پشتیبانی می کند.

## دانلود / Download

### GitHub Releases (Portable ZIP)

[Download](https://github.com/wanqiua/nekoray/releases)

اگر هنوز نسخه‌ای منتشر نشده است، طبق [راهنمای ساخت](docs/readme.md) پروژه را کامپایل کنید.

### بسته‌های پروژه بالادستی / Upstream packages

1. AUR [nekoray-git](https://aur.archlinux.org/packages/nekoray-git)
2. Scoop Extras: `scoop install nekoray`
3. Homebrew (unoffical): [homebrew-nekoray](https://github.com/tdjnodj/homebrew-nekoray)
4. APT (unoffical): [tdjnodj/apt](https://github.com/tdjnodj/apt)

## پروکسی / Proxy

- SOCKS (4/4a/5)
- HTTP(S)
- Shadowsocks
- VMess
- VLESS
- Trojan
- NaïveProxy ( Custom Core )
- Hysteria ( Custom Core or sing-box )
- Hysteria2 ( Custom Core or sing-box )
- Custom Outbound
- Custom Core

## اشتراک / Subscription

- فرمت های خام: برخی از فرمت های پرکاربرد (مانند Shadowsocks، Clash و v2rayN)

## پارامترهای عملیاتی

- `-many` نمونه در حال اجرا همان دایرکتوری را نادیده بگیرید و به اجبار یک نمونه جدید راه اندازی کنید(0.11+)
- `-appdata` پس از باز کردن، فایل پیکربندی در دایرکتوری مشترک قرار می گیرد و نمی توان آن را بیشتر باز کرد و به طور خودکار ارتقا داد. (0.11+)

## Windows 

اگر به شما می گوید که DLL وجود ندارد و نمی تواند اجرا شود، لطفاً [Microsoft C++ Runtime Library] (https://aka.ms/vs/17/release/vc_redist.x64.exe) را دانلود و نصب کنید.

## Linux 

[آموزش اجرای لینوکس](docs/Run_Linux.md)

## آموزش کامپایل

[Linux](docs/Build_Linux.md)

[Windows](docs/Build_Windows.md)

[macOs](docs/Build_MacOS_fa.md)

## Credits

- [v2fly/v2ray-core](https://github.com/v2fly/v2ray-core)
- [MatsuriDayo/Matsuri](https://github.com/MatsuriDayo/Matsuri)
- [MatsuriDayo/v2ray-core](https://github.com/MatsuriDayo/v2ray-core)
- [SagerNet/sing-box](https://github.com/SagerNet/sing-box)
- [Qv2ray](https://github.com/Qv2ray/Qv2ray)
- [Qt](https://www.qt.io/)
- [protobuf](https://github.com/protocolbuffers/protobuf)
- [yaml-cpp](https://github.com/jbeder/yaml-cpp)
- [zxing-cpp](https://github.com/nu-book/zxing-cpp)
- [QHotkey](https://github.com/Skycoder42/QHotkey)
- [AppImageKit](https://github.com/AppImage/AppImageKit)
