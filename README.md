# HQMarket

HQMarket 是独立的 C++20 行情数据服务。服务通过 CPython 3.12 嵌入
MooTDX 和 AKShare，并以 TCP/Protobuf 提供实时订阅，以 HTTP 提供健康检查、
指标和历史查询。

## 端口与接口

- TCP `9901`：`4-byte big-endian length + MarketEnvelope`。
- HTTP `9902`：`/health`、`/metrics`、`/v1/instruments`、`/v1/quotes?instrument=600519.SSE`、`/v1/bars?instrument=600519.SSE`。
- 协议版本：`market/v1/market.proto`，当前 `1.0`。

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
