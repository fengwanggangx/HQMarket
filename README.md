# HQMarket

HQMarket 是独立的 C++20 行情数据服务。服务通过 CPython 3.12 嵌入
MooTDX 和 AKShare，并以 TCP/Protobuf 提供实时订阅，以 HTTP 提供健康检查、
指标和历史查询。

## 端口与接口

- TCP `9901`：`4-byte big-endian length + CRequest(RequestData)`。
- HTTP `9902`：`/health`、`/metrics`、`/v1/instruments`、`/v1/quotes?instrument=600519.SSE`、`/v1/bars?instrument=600519.SSE`。
- 通信协议：`request/request.proto`；具体行情消息定义位于 `quote/v1/market.proto`。

TCP 客户端认证后可通过 `CRequest.cmd` 发送 `auth`、`subscribe`、`unsubscribe`、`heartbeat`、`query_quote`、`query_bars`。
响应及推送的具体 protobuf 由 `CData` 保存，序列化后映射到 `CRequest.ret["data_type"]`
和 `CRequest.ret["data"]`；关联与推送元数据位于 `ret` 中的 `request_id`、`sequence`、
`server_time_ms`。订阅或退订
`CHANNEL_QUOTE`、`CHANNEL_DEPTH`。服务返回逐项 `SUBSCRIPTION_ACK`，订阅最新价时若缓存已有
快照，会在 ACK 后立即发送一条 `QUOTE`，之后持续推送实时数据。

报价查询使用 `cmd=query_quote`，K 线查询使用 `cmd=query_bars`，参数通过 `CRequest.extra`
传递。服务以相同 `request_id` 返回 `QueryResponse` 数据；K 线优先查 SQLite，无数据时
回源 AKShare 并写入缓存库。

## 运行

HQMarket 要求随服务部署 `runtime/python`，并在 `ini/system.ini` 中配置访问令牌：

```ini
[HQMarket]
token=replace-with-a-random-token
home=/opt/hqmarket
```

`home` 可留空；程序会依次使用 `HQMARKET_HOME` 环境变量和当前工作目录作为回退。

使用 `deploy/stage-python.sh` 在Linux x64发布目录中创建固定Python 3.12运行时，
使用 `deploy/package-contract.ps1` 生成协议包。Wind不属于本仓库，本阶段不会修改。

## 构建与测试

正式程序通过 `HQMarket.sln` 中的 `HQMarket/HQMarket.vcxproj` 进行 VS Linux 远程构建。目标机需要 Python 3.12 开发文件和 SQLite3 开发库。

根目录 CMake 仅转接 `HQMarket/system/tests` 的离线路由测试，不再保留已删除的 CoreTests/CFrameCodec 测试目标。测试依赖及可选命令见下文；不作为正式程序构建入口。

## 服务分层

`CBootLoader` 创建并持有网络端点、PythonRuntime、`CHQService` 和 `CHQCenter`，统一绑定 TCP/HTTP 入口和行情回调。`CHQCenter` 只借用行情服务指针，管理认证、心跳、请求转发和网络发送；查询、订阅关系、行情转换、缓存和数据源由 `CHQService` 及其持有的 `CHQBroker` 管理。行情源回调经中心转交服务，服务通过固定响应回调向指定 client 回包或推送，不直接调用网络发送接口。

查询仍采用同步返回，保留原请求 ID；行情推送的请求 ID 为 0。订阅共享、ACK 后补发缓存快照、HTTP 路径和响应格式保持原有行为。退出时先停止网络循环并排空 TCP 异步请求、清除入口回调，再停止行情源并关闭记录器，解绑和销毁中心、服务，最后释放网络、数据库和 PythonRuntime。

离线路由测试使用真实中心、服务、行情聚合层、缓存及 SQLite 记录器，替换 TCP/HTTP 传输、PythonRuntime 和行情源边界，不连接外部行情或 MySQL。可单独构建：

```sh
cmake -S HQMarket/system/tests -B build-routing -DHQMARKET_DEPS=/path/to/dependency-bundle
cmake --build build-routing
ctest --test-dir build-routing --output-on-failure
```

依赖目录结构为 `include/debug` 和 `libs/debug`，可通过 `HQMARKET_MODE` 选择其他配置；Windows 运行时还需将对应依赖 DLL 所在目录加入 PATH。

## 正式程序构建入口

正式程序统一从 Visual Studio 解决方案构建，以 `.vcxproj` 为准。仓库中的 CMake 入口仅用于辅助验证，不作为正式程序构建、发布或验收依据。

打开 `HQMarket.sln`，选择 `HQMarket/HQMarket.vcxproj`，使用 VS Linux 远程构建，配置为 Debug 或 Release、平台 x64；需在 VS 中配置 Linux 连接及工程所需依赖。
本次仅核对解决方案及工程文件，不执行编译、链接或运行测试。
