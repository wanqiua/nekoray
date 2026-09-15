# NekoRay_BY_WanQiu 0.1

本版使用独立产品名 **NekoRay_BY_WanQiu** 和版本号 **0.1**，标签为 `v0.1`。Windows 主程序为 `NekoRay_BY_WanQiu.exe`。上游 GUI 基线 3.26 只保留为来源信息；旧标签和发行文件保留原样。

## 问题与修复

| 问题 | 排查结果 | 0.1 的处理 |
| --- | --- | --- |
| 当前分组删除不可用配置没有反应 | 旧入口只接受带有效来源信息的 URL 失败，TCP/综合测试失败不参与判断；没有候选项时直接返回。 | 优先采用有效 URL 结果，URL 未测试时采用同一配置和内核的最新有效 TCP/综合测试结果。删除前确认数量；无候选项时解释原因。保留正在连接、未测试、过期、内核错误和其他分组的配置。 |
| 良心云 VLESS 不能选 Xray，参数仍旧 | Clash 导入器为 VLESS 设置 `xudp`，能力检查却拒绝 Xray 的所有非空 `packet_encoding`。另有 Hysteria2 导入失败，导致整组订阅保持旧内容。 | 为 VLESS XUDP 生成 Xray 对应的 UDP 复用配置，并在启用 TCP 复用时保留这些字段。 |
| 21 个 Hysteria2 因 `fingerprint` / `mport` 无法导入 | 导入器尚未适配这两个字段。11 项同时提供相同的 `ports` 与 `mport`。完整导入还暴露了缺省跳端口间隔被转换成字符串 `null` 的问题。 | 支持证书 SHA-256 和跳端口别名；缺省字段保留默认值。规范化后相同的端口范围可重复提供，冲突仍报错并保留旧分组。 |
| sing-box 广告阻断出现权限错误 | 原生 `block` 出站返回 `operation not permitted`，按连接错误输出日志。 | 将直接指向阻断出站的路由及最终路由转换为 `reject` 动作，保留实际阻断行为。 |
| 选择节点使顶部高度变化 | “下次连接”和原因标签动态添加内容并换行。 | 删除这段标签；内核按钮保持单行，自动按钮显示目标内核，原因放在悬停提示中。 |
| 名称与版本仍是 3.26 | 产品版本沿用上游基线。 | 窗口、文件属性、内核构建标识及发行包统一使用新名称和 0.1。资源名、配置目录和内核文件名继续兼容。 |

有效测速结果仍绑定内核构建、配置指纹、测试模式及作用范围。旧版无来源信息的负延迟不直接作为删除依据；升级后应先重新测试当前分组。

## 良心云 UA 对比

2026-09-15 对同一订阅进行了只读对比，未修改正在使用的配置：

| UA | 返回格式 | 比较结果 |
| --- | --- | --- |
| `NekoRay/PC/3.26 (Prefer ClashMeta Format)` | Clash YAML | 36 个 VLESS、21 个 Hysteria2。 |
| `NekoBox/PC/3.26 (Prefer ClashMeta Format)` | Clash YAML | 与上一响应字节相同。 |
| `NekoRay/PC/3.26` | Base64 节点链接 | 同样的协议数量，传输参数等价。 |
| `NekoRay/PC/0.1 (Prefer ClashMeta Format; NekoRay_BY_WanQiu)` | Clash YAML | 仍为 57 项；与先前 YAML 相比只有一项显示名称变化，去除名称后所有节点参数一致。 |

这组结果表明服务端会根据 UA 协商返回格式，本次未发现上述 UA 改变节点传输参数。不能据此保证服务端对所有其他 UA 或未来订阅都采用相同行为。多次请求曾触发 HTTP 429；该响应没有作为节点参数证据。

新默认 UA 保留 ClashMeta 格式协商标识，切换默认内核不会再改变 UA。用户自定义 UA 继续优先。订阅 URL、节点地址和凭据不进入源码包或发行附件。

## Hysteria2 证书指纹

Clash 的 `fingerprint` 和 Hysteria2 链接的 `pinSHA256` 表示叶证书 DER 的 SHA-256。它们不同于浏览器/uTLS 指纹，也不同于 sing-box 的 `certificate_public_key_sha256` 公钥哈希。

本版保留指纹到导入、编辑、保存、分享和订阅更新，并由随包 `nekobox_core.exe` 实际验证。支持 64 位十六进制或冒号分隔格式。即使开启跳过常规证书验证，已配置的证书指纹仍须匹配；未开启时还须通过原有证书校验。固定指纹的连接关闭 TLS 会话恢复，确保每次连接都重新检查证书。

该适配保留 Hysteria2 的 Gecko、端口跳跃、带宽与 QUIC 参数。未设置指纹时仍使用上游构造器。自定义配置将证书指纹与 Realm 同时使用会明确报错；本次订阅未使用 Realm。导出给官方 sing-box 的配置不能直接使用本包装器的 `tls_certificate_sha256` 扩展；外置 Hysteria2 使用其原生 `pinSHA256`。

## 验证与范围

回归覆盖真实菜单删除、空结果提示、顶部高度、订阅字段与分享保存、证书正确/错误指纹、Xray VLESS TCP/WebSocket/Vision 的 TCP 与 UDP、sing-box 正常阻断及完整订阅导入。包验收核对实际 EXE、依赖、源码和哈希，并用独立合成配置验证升级保存、重启、原生 GUI 退出与 TLS 正反例。

本次的通过数量、最终包哈希和验证限制记录在发行附件 `NekoRay_BY_WanQiu-0.1-verification.md`。仓库中已有的系统代理、TUN 和旧发行包验收记录保留各自日期与哈希，不当作本次新二进制的重复验证。订阅导入及本地协议回环通过不代表已逐个连接和测速全部远端节点。

## 升级

先退出旧版，备份并复制旧版 `config` 文件夹到新程序目录，再运行 `NekoRay_BY_WanQiu.exe`。无需重新导入已有节点和订阅。良心云可重新更新订阅，导入此前因未适配参数而保留在旧内容中的新增节点。

下载：[v0.1 发行页面](https://github.com/wanqiua/nekoray/releases/tag/v0.1)。使用说明见 [Windows 便携版](Portable_Windows.md)，源码构建见 [Windows 构建](Build_Windows.md)。
