# 构建双内核

这个开发分支以 Nekoray **3.26** 为基础，保留 Xray / sing-box 两种后端。4.x 已移除 Xray，不能直接作为双内核版本的界面替代品。

| 组件 | 固定版本 | Go module |
| --- | --- | --- |
| Xray | [Matsuri v1.8.4 兼容版](https://github.com/MatsuriDayo/Xray-core/commit/01208225ee7e508044cca8eb6776a117bcecd997) | `github.com/xtls/xray-core v1.8.4`，替换到 `github.com/MatsuriDayo/Xray-core v0.0.0-20230829080406-01208225ee7e` |
| sing-box | [v1.14.0](https://github.com/SagerNet/sing-box/releases/tag/v1.14.0) | `github.com/sagernet/sing-box v1.14.0` |
| Go（Xray） | 1.21.13 | 兼容旧 QUIC / REALITY 依赖 |
| Go（sing-box） | 1.26.4 | sing-box v1.14.0 工具链 |

版本在 `go.mod` / `go.sum` 中固定。Xray 使用 Matsuri fork 以保留 `rc4-md5`、旧 Shadowsocks 插件和 `allowInsecure`；sing-box 使用官方模块。Go 会下载固定依赖，无需另外检出这些内核仓库。

## Windows / PowerShell

安装 Go 1.26.4 和 Git，在仓库根目录执行。脚本通过 `GOTOOLCHAIN` 为两个内核分别选择上述工具链，首次使用会自动下载缺少的版本：

```powershell
.\libs\build_cores.ps1 -Test
```

输出位于 `deployment/windows64/`：

- `nekoray_core.exe`：带原 GUI gRPC 接口的 Xray。
- `nekobox_core.exe`：带原 GUI gRPC 接口和旧配置迁移层的 sing-box。
- `core-versions.json`：模块版本、实际替换来源、Xray 固定提交、各自工具链、目标平台和 SHA-256。

只重建内核可省略 `-Test`。这一步不需要 Qt、CMake，也不重建 GUI。

## Linux / Git Bash

```bash
bash libs/get_source.sh
NKR_TEST_CORES=1 bash libs/build_go.sh
```

脚本默认构建当前平台；可以通过 `GOOS` / `GOARCH` 指定交叉编译目标，例如：

```bash
GOOS=windows GOARCH=amd64 bash libs/build_go.sh
GOOS=linux GOARCH=amd64 bash libs/build_go.sh
```

交叉编译时不要设置 `NKR_TEST_CORES=1`，测试需在对应平台运行。Bash 脚本输出两个内核；版本与哈希清单由 PowerShell 脚本生成。

sing-box 的功能标签统一保存在 `libs/core_build_tags.txt`，包含 QUIC、WireGuard、uTLS、gVisor、Clash API、V2Ray API 和 DHCP。Linux 还会构建便携运行需要的 `launcher`；其他平台的旧更新器默认不构建。仅在维护者已更换更新源时使用 `NKR_BUILD_UPDATER=1`。

## 使用原版数据库进行回归测试

默认测试使用本地回环网络，不需要外部代理节点。若要同时验证 3.26 自带的 `geoip.db` 和 `geosite.db`，把官方便携包解压到单独目录，并指向其中包含这两个文件的目录：

```powershell
$env:NEKORAY_TEST_ASSET_DIR = 'C:\path\to\nekoray-3.26\nekoray'
.\libs\build_cores.ps1 -Test
```

未设置这个环境变量时，依赖原版数据库的测试会跳过；其他回归测试仍会执行。测试不会启用系统代理或接管系统 TUN 路由。

## 在 GUI 中使用

本分支的批测调度、分组右键菜单和导入窗口需要重新编译 GUI，步骤见 [Windows GUI 构建](Build_Windows.md)。以上 Go 构建命令只生成内核。

如果只验证双内核兼容性，可以在独立的 [Nekoray 3.26 Windows 便携副本](https://github.com/MatsuriDayo/nekoray/releases/tag/3.26) 中替换两个同名内核，保留其 Qt 运行库和数据库。原版 GUI 不包含本分支的批测与界面改动。

GUI 和内核通过专用 gRPC 接口通信，因此不能直接把官方 `xray.exe` 或 `sing-box.exe` 改名后替换。新增协议字段的表单和分享链接解析需另行扩展，可使用自定义出站 / 自定义配置。

本分支禁用了旧的上游自动更新接口，避免更新到移除了 Xray 的 4.x。配置迁移范围、已验证行为和限制见 [升级说明](Core_Upgrade.md)。

验证已构建的两个内核进程及共享 URL 测试逻辑：

```powershell
$env:NEKORAY_CORE_BIN_DIR = (Resolve-Path deployment/windows64).Path
Push-Location go/grpc_server
go test -count=1 -timeout 75s ./...
Pop-Location
```

## 继续修改内核

适配层入口是 `go/cmd/nekoray_core/` 和 `go/cmd/nekobox_core/`。一般的 GUI 配置兼容问题在这两处解决；协议实现分别在固定的 Matsuri Xray 和官方 sing-box 模块中。手动运行 Xray 的 Go 命令前需设置 `GOTOOLCHAIN=go1.21.13`，较新的宿主 Go 不会仅凭 `toolchain` 指令自动降级。

需要修改协议源码时，先把对应来源仓库及固定提交检出到自己的开发目录，再在对应 Go 模块中用 `go mod edit -replace=模块路径=本地目录`，或者使用本地 `go.work`。本地替换不会自动纳入发布版本；交付前固定到自己的 fork 提交，重新构建并运行回归测试。

升级后续版本时，分别更新两个模块的依赖、执行 `go mod tidy`，然后重新运行上面的构建和测试。不要跳过旧配置迁移、RC4-MD5、跳过证书验证及批量 URL 测试：直接改回最新官方 Xray 会再次失去所需的旧功能。兼容版不包含 XHTTP 等后续 Xray 功能。
