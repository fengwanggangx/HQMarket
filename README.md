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

## 全市场扫描试运行

在测试实例的进程环境设置 `HQMARKET_ENABLE_MARKET_SCAN=1` 后启动 HQMarket。服务会批量扫描已加载的证券目录，在 SQLite 保存最近一次发布的轮次。快照返回实际采集起止时间、有效报价数和目录标的数，供调用方判断覆盖情况。

启用扫描后，后台每五分钟检查证券目录。北京时间换日或本地补充名录文件的修改时间变化时触发刷新。刷新失败时保留原目录并在下一次检查时重试，新目录在扫描轮次边界生效。

深圳 ETF 和 LOF 可使用交易所清单导出的 `ini/szse_etf.csv` 和 `ini/szse_lof.csv`。每个文件用 UTF-8 编码，不含表头，每行依次填写六位证券代码和证券简称。有效文件优先用于对应的深圳基金类别；缺失或校验失败时尝试东方财富目录。部署方负责更新这两个文件。

公募 REITs 归入基金扫描范围。上交所使用交易所公募 REITs 分类，深交所使用官方不动产基金清单导出的 `ini/szse_reits.csv`，文件格式与深圳 ETF、LOF 名录相同。

沪深北指数可分别通过 `ini/sse_indices.csv`、`ini/szse_indices.csv` 和 `ini/bse_indices.csv` 补充行情源缺少的目录项。文件按交易所目录导出，每行填写六位代码和指数名称。

港交所官方名录中的活跃证券可通过 `ini/hk_supplement.csv` 补充到港股扫描目录，每行填写五位代码及名称。港股盘中扫描可用备用源补齐缺失或无效报价，备用记录须为同日报价且有成交。

如转债行情源仍列出已确认退出上市范围的证券，可在 `ini/inactive_convertible_bonds.txt` 中按行填写 `代码.交易所`，并用 `#` 添加证据说明。名单在刷新证券目录时读取，适用于有交易所或发行人证据的退出记录。

- HTTP 使用 `GET /v1/market/snapshot`。TCP 使用 `MARKET_QUOTE_QUERY`，请求 `extra.view=market_snapshot`，响应从 `extra.market_snapshot` 读取同结构的 JSON。
- TCP 客户端也可在 `MARKET_QUOTE_QUERY` 的请求中填入 Protobuf `market_snapshot_query`，并从响应的 `market_snapshot_page` 读取分页和行情。两种 TCP 请求形式使用相同的筛选、分榜和轮次规则。
- `scope` 可选 `a_share`、`index`、`fund`、`convertible_bond` 或 `hk`。指定 `board=gainers`、`losers` 或 `turnover` 查询榜单，默认榜单范围为 A 股。用 `partial=1` 显式允许部分覆盖榜单。
- 下一页沿用首屏的 `version`、`scope`、`board` 和 `partial`，以 `next_offset` 作为 `offset`。`limit` 默认 100，最大 500。收到 `version_expired` 时重新读取首屏。
- 默认快照返回境内轮次，港股用 `scope=hk` 查询独立发布和恢复的轮次。`prior_date=true` 表示轮次来自较早的本地自然日，该轮次仍可查快照，榜单显示准备中。
- 将独立上市清单导出到执行目录的 `ini/market_reference_cn.txt` 和 `ini/market_reference_hk.txt`。首行写 `YYYYMMDD`，后续每行写一条扫描范围内的 `代码.交易所`，例如 `600519.SSE`。每轮对账，日期和证券集合一致才返回 `directory_verified=true`。清单缺失或不一致时，榜单需用 `partial=1` 显式查看试运行结果。
- 正式部署可在执行目录下提供 `ini/market_sessions.txt`。每行以空白分隔日期 `YYYYMMDD`、市场 `CN` 或 `HK`、从北京时间午夜起算的开始分钟和结束分钟。一个市场每天可有多行，结束分钟不包含在区间内。节假日省略，半日市只填写实际时段，可在最后一个区间预留收盘补采时间。文件每天首次运行时读取。缺少文件时使用普通工作日时段，适用于试运行。

- 港股参考清单每条证券后填写 `stock` 或 `other`，例如 `00700.HKEX stock`。港股榜单只收录已核验的股票。同日参考清单与行情目录有差异时，`partial=1` 可查看其中已核验的股票，常规榜单仍要求完整对账。境内和港股记录都可在末尾追加 `halted` 表示已确认停牌，停牌不计入缺失数并从榜单排除。
- 参考清单可以附加 `pre_close=123400`，表示经过核验的当日参考昨收为 12.3400。若确认当天需要使用参考昨收、但尚无可靠值，填 `pre_close=unknown`。该标的仍可查询快照，但不参与涨跌幅排名。修正随快照入库，供同一轮分页和榜单使用。

## 正式程序构建入口

正式程序统一从 Visual Studio 解决方案构建，以 `.vcxproj` 为准。仓库中的 CMake 入口仅用于辅助验证，不作为正式程序构建、发布或验收依据。

打开 `HQMarket.sln`，选择 `HQMarket/HQMarket.vcxproj`，使用 VS Linux 远程构建，配置为 Debug 或 Release、平台 x64；需在 VS 中配置 Linux 连接及工程所需依赖。
本次仅核对解决方案及工程文件，不执行编译、链接或运行测试。
