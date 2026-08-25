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

HQMarket要求随服务部署的 `runtime/python` 和环境变量：

```bash
export HQMARKET_HOME=/opt/hqmarket
export HQMARKET_TOKEN='replace-with-a-random-token'
./HQMarket
```

使用 `deploy/stage-python.sh` 在Linux x64发布目录中创建固定Python 3.12运行时，
使用 `deploy/package-contract.ps1` 生成协议包。Wind不属于本仓库，本阶段不会修改。

## 构建与测试

Visual Studio Linux项目位于 `HQMarket.sln`。目标机需要Python 3.12开发文件和
SQLite3开发库。与外部依赖无关的核心测试可通过CMake构建：

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```
