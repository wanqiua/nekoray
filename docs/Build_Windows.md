# 在 Windows 下构建本分支

本分支以 3.26 为基础，包含 Qt GUI、Xray 和 sing-box 三个可执行文件。GUI 与内核都要重建，才能使用批测和分组操作的完整改动。编译、测试与便携包打包步骤分别列在下方。

本地验证使用 Qt 6.5.2 MSVC x64 SDK、MSVC 14.51、ZXing 2.0.0、yaml-cpp 0.7.0 和 protobuf 21.4。较新的 MSVC 删除了旧 Qt 引用的 `stdext` 迭代器，本分支通过局部编译兼容头处理，无需改动 Qt SDK 文件。

### 获取源码

```
git submodule update --init --recursive
```

### 安装 Visual Studio

在本分支仓库中工作，初始化固定版本子模块。从微软官网安装 Visual Studio C++ 桌面开发组件、Windows SDK、CMake 和 Ninja。

安装好后可以在「开始」菜单找到 `x64 Native Tools Command Prompt`

以下 PowerShell 示例应在 Visual Studio 开发者 PowerShell 中执行。`cmake` 和 `ninja` 可以使用 VS 自带版本。

### 下载 Qt SDK

目前 Windows Release 使用的版本是 Qt 6.5.x

下载解压后，将 bin 目录添加到当前终端的 PATH。Qt SDK 需要包含 Core、Gui、Widgets、Network、Svg、LinguistTools；GUI 回归测试还需要 Qt Test。

#### Release 编译用到的 Qt 包下载 (MSVC2019 x86_64)

https://github.com/MatsuriDayo/nekoray_qt_runtime/releases/download/20220503/Qt6.5.0-Windows-x86_64-VS2022-17.5.5-20230507.7z

#### 官方签名版 Qt 5.15.2 （可选，已知有内存泄漏的BUG）

在此下载 `qtbase` `qtsvg` `qttools` 的包并解压到同一个目录。

https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt5_5152/qt.qt5.5152.win64_msvc2019_64/

### C++ 部分编译

#### 编译安装 C/C++ 依赖

（这一步可能要挂梯）

```shell
bash ./libs/build_deps_all.sh
```

目前只有 bash 脚本，没有批处理或 powershell，如果 Windows 没有带 bash 建议自行安装。

CMake 参数等细节与 Linux 大同小异，有问题可以参照 Build_Linux 文档。

#### 编译本体

请把示例中的 Qt SDK 和依赖安装目录换成实际路径。`NKR_BUILD_TESTS` 用于构建本分支的 GUI 回归程序；仅编译 GUI 时可省略。

```powershell
$qtSdk = 'C:/path/to/Qt6.5.2'
$cppDeps = 'C:/path/to/cpp-install'
$env:PATH = "$qtSdk/bin;" + $env:PATH
$env:QT_PLUGIN_PATH = "$qtSdk/plugins"

cmake -S . -B build/gui -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 `
  -DQT_VERSION_MAJOR=6 `
  "-DCMAKE_PREFIX_PATH=$qtSdk" `
  "-DNKR_LIBS=$cppDeps" `
  -DNKR_BUILD_TESTS=ON
cmake --build build/gui --parallel 8
```

编译完成后得到 `build/gui/nekoray.exe`。Qt 运行时的主版本、编译器和位数必须与 SDK 匹配。

开发验证直接使用当前 PATH 中的 Qt 运行库即可。

### Go 部分编译

请看 [Build_Core.md](./Build_Core.md)

### 本地回归测试

按内核构建教程安装 Go 1.26.4；构建脚本为兼容版 Xray 自动选择 Go 1.21.13，sing-box 使用 Go 1.26.4。先构建两个内核，再放入 GUI 测试程序所在目录：

```powershell
.\libs\build_cores.ps1 -Test
Copy-Item deployment/windows64/nekoray_core.exe, deployment/windows64/nekobox_core.exe build/gui/
ctest --test-dir build/gui --output-on-failure
```

包括 `gui_ray`、`gui_box` 和 `speedtest_batch`。GUI 日志保存在构建目录的 `gui_ray.log` 和 `gui_box.log`。测试操作临时配置目录和本地 HTTP 代理节点，不读取用户配置、不启用系统代理或 TUN。

测试验证批测与逐个测试的判定一致、真实并发、取消、旧结果归属，以及分组菜单和链接导入流程。需要检查中文布局时，可在运行测试前将 `NEKORAY_TEST_SCREENSHOT_DIR` 设为绝对目录；测试会保存使用示例 URL 的界面截图。

只运行无需 Qt 的队列调度测试：

```powershell
cmake -S tests -B build/batch-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/batch-tests
ctest --test-dir build/batch-tests --output-on-failure
```

正常启动 GUI 时仍需为分流规则提供对应的 GeoIP / Geosite 数据库。原版 3.26 数据库的迁移测试方法见内核构建教程。

### Windows x64 便携包

在 PowerShell 7 中调用 `libs/package_windows.ps1`。需要 Git、7-Zip、构建 GUI 时使用的 Qt SDK，以及 MSVC x64 可分发运行库目录。数据库目录应包含 `geoip.dat`、`geosite.dat`、`geoip.db` 和 `geosite.db`；本次使用官方 3.26 便携包中的文件。

先整理依赖许可证目录（包含 `README.txt` 来源清单、Qt、OpenSSL、C++ 依赖、两套 Go 工具链和内核 Go 依赖的许可证），将其传入 `NoticesDirectory`。Go 模块含 `replace` 时按实际替换来源收集许可证；本次 Xray 来源为 Matsuri fork。脚本只将已核对的构建与指定资源装入全新的临时目录，不使用旧部署目录中的其他文件。

```powershell
.\libs\package_windows.ps1 `
  -GuiPath build/gui/nekoray.exe `
  -QtSdk $qtSdk `
  -CrtDirectory 'C:/path/to/VC/Redist/MSVC/version/x64/Microsoft.VC145.CRT' `
  -GeoDataDirectory 'C:/path/to/nekoray-3.26/nekoray' `
  -NoticesDirectory 'C:/path/to/package-notices'
```

输出到 `deployment/packages/`：便携 ZIP、对应源码 ZIP 和 SHA-256 文件。源码快照包含未提交修改、新增源码及固定版本的 QHotkey / qjs 子模块。脚本会核对内核哈希与 ZIP 完整性；同名输出已存在时会停止，可通过 `-PackageName` 指定新的名称。

交付前还需解压便携 ZIP，在清除开发用 Qt 环境变量的情况下用临时 `-appdata` 目录分别启动两种内核，并核对实际加载的 DLL 路径。运行 GUI 回归程序时可向这个测试副本临时加入 `Qt6Test.dll` 和 `platforms/qoffscreen.dll`，这些测试文件无需打入便携 ZIP。
