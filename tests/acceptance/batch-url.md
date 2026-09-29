# 混合内核 URL 批测验收

在仓库目录运行：

```powershell
.\tests\acceptance\batch-url.ps1
```

默认使用 `build/dynamic-core-check/bin` 中的包装内核，执行 3 轮。验证本次升级请通过 `-CoreDirectory deployment/windows64` 指向新构建的 wanqiua Xray **26.3.27-matsuri.1** 和 sing-box **1.14.0**。每轮使用 24 个合成节点，合计并发为 6，成功服务端固定等待 200 ms 后返回响应。需要已安装 Go；脚本固定使用 `go1.26.4` 和 `-mod=readonly`，不会升级项目依赖或内核。

可指定隔离构建位置和并发参数：

```powershell
.\tests\acceptance\batch-url.ps1 -CoreDirectory .\build\dynamic-core-check\bin -Concurrency 12 -NodeCount 48 -Repeat 3
```

结果保存在 `build/dynamic-core-check/batch-acceptance/<UTC 时间>/`，包含每轮 JSON、原始测试日志和 `summary.json`。每轮校验实际可执行文件的 SHA-256、内核家族、版本，以及每条非取消 RPC 结果的内核身份。输出仅包含合成节点标识和测试结果，不包含 RPC 令牌或代理密码。

## 验收内容

- **状态一致性**：同一组合成节点先串行、再混合批测，按节点 ID 和内核核对结果及错误分类。覆盖快速成功、延迟成功、代理拒绝 CONNECT、目标返回 503、目标不响应以及本地配置错误。失败样本是故意注入的；验收要求正确识别，不能把它们都变成“节点超时”。
- **实际代理路径**：每个节点有自己的回环 HTTP CONNECT 服务、认证信息和 URL 路径。目标地址固定为 `127.0.0.1:1`，服务端直接提供测试响应，不向目标转发。必须命中该节点的代理入口，直接回退或串用节点配置都会失败。
- **统一并发**：一个 worker 队列同时调度 Xray 和 sing-box，每个 worker 使用独立 RPC 通道。分别记录客户端在途请求数、服务端真实活动请求总峰值和各内核峰值。合计峰值不得超过配置值，成功批次必须填满该预算。
- **耗时与吞吐**：记录串行及混合批次总耗时、每秒完成数和加速倍数。采用固定服务端延迟降低本机抖动的影响；默认要求至少 2 倍串行吞吐。并发为 2 时门槛为 1.2 倍。
- **取消与恢复**：让整个并发预算里的请求同时阻塞，随后取消。检查未开始任务不再发出、活动连接关闭、全部任务标为取消，且 3 秒内收尾。之后直接复用原来的两个内核进程重新测试，要求全部成功。
- **进程清理**：结束时只向本轮持有的进程发送 Exit；保留进程对象作为异常清理后备，不按进程名终止程序。

延迟数值不要求串行和批测完全相同，网络和调度耗时正常会有波动。验收比较状态及错误类别，延迟必须是有效正值。

各内核的峰值可能出现在不同时间，不能把两列峰值相加当作全局峰值；全局值以 `server_peak` 为准。当前实测结果见 [验收记录](batch-url-results.md)。

## 验证范围

入口为 `go/grpc_server/testutil/batch_acceptance_test.go::TestMixedCoreBatchAcceptance`。它验证真实包装内核和 RPC 传输，不替代 `gui_regression_test` 对 Qt 批测调度、取消按钮和结果保存的验证。每秒完成数是受控回环 URL 探测吞吐，不代表节点下载带宽或公网质量。

测试只启动普通权限进程和 `127.0.0.1` 监听器，不加载用户节点，不修改用户配置、系统代理或 TUN。每个内核使用新的临时工作目录；脚本退出会恢复临时设置的环境变量。

在其他平台可直接调用相同测试：

```sh
GOTOOLCHAIN=go1.26.4 \
NEKORAY_CORE_BIN_DIR=/absolute/path/to/isolated/cores \
NEKORAY_BATCH_REPORT=/absolute/path/to/report.json \
go test -mod=readonly -v -count=1 -run '^TestMixedCoreBatchAcceptance$' -timeout 180s ./testutil
```

以上命令的工作目录为 `go/grpc_server`。环境变量 `NEKORAY_BATCH_NODES`、`NEKORAY_BATCH_CONCURRENCY`、`NEKORAY_BATCH_DELAY_MS` 可设置相同参数。未指定核心目录时，该测试跳过，不会尝试调用已有安装。
