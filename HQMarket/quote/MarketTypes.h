#ifndef __MARKET_TYPES_H__
#define __MARKET_TYPES_H__
#include <cstdint>
#include <string>
#include <vector>
namespace market
{
	// 交易所或市场类型。
	enum class Exchange
	{
		unknown = 0, // 未知市场。
		sse,         // 上海证券交易所。
		szse,        // 深圳证券交易所。
		bse,         // 北京证券交易所。
		hkex,        // 香港交易所。
		cffex,       // 中国金融期货交易所。
		shfe,        // 上海期货交易所。
		dce,         // 大连商品交易所。
		czce,        // 郑州商品交易所。
		ine,         // 上海国际能源交易中心。
		gfex,        // 广州期货交易所。
		nasdaq,      // 纳斯达克证券交易所。
		nyse,        // 纽约证券交易所。
		crypto       // 数字货币市场。
	};

	// 行情数据通道或数据周期。
	enum class Channel
	{
		unknown = 0, // 未知数据类型。
		quote,       // 最新行情快照。
		depth,       // 买卖盘口深度。
		trade,       // 逐笔成交。
		bar_1m,      // 一分钟K线。
		bar_1d,      // 日K线。
		market_status // 市场开盘、休市或收盘等状态。
	};

	struct CSecurity
	{
		std::string m_strCode;
		Exchange m_market{ Exchange::unknown };

		CSecurity() = default;
		CSecurity(const std::string& strCode, market::Exchange mk);
		CSecurity(const CSecurity& arg);
		CSecurity& operator=(const CSecurity& arg);
		bool operator==(const CSecurity& arg) const;

		bool IsValid() const;
		std::string String() const;
	};

	std::string GetMarketString(Exchange mk);
	std::string FmtSecurityString(const std::string& strCode, Exchange mk);

	// 盘口中的一个价格档位；价格采用定点整数表示。
	struct CPriceLevel
	{
		std::int64_t m_nPrice{ 0 };  // 档位价格，实际值为m_nPrice / 10^m_nPriceScale。
		std::int64_t m_nVolume{ 0 }; // 档位委托数量，具体单位由数据源约定。
		int m_nPriceScale{ 4 };       // 价格的小数位数。
	};

	// 单个标的的最新行情快照。
	struct CQuote
	{
		CSecurity m_security;                  // 标的信息。
		std::int64_t m_nExchangeTime{ 0 };     // 交易所生成行情的毫秒时间戳。
		std::int64_t m_nReceiveTime{ 0 };      // 服务接收行情的毫秒时间戳。
		std::int64_t m_nLastPrice{ 0 };        // 最新成交价。
		std::int64_t m_nOpenPrice{ 0 };        // 当日开盘价。
		std::int64_t m_nHighPrice{ 0 };        // 当日最高价。
		std::int64_t m_nLowPrice{ 0 };         // 当日最低价。
		std::int64_t m_nPreClose{ 0 };         // 前一交易日收盘价。
		std::int64_t m_nVolume{ 0 };           // 累计成交量，具体单位由数据源约定。
		std::int64_t m_nTurnover{ 0 };         // 累计成交额，具体单位由数据源约定。
		int m_nPriceScale{ 4 };                 // 所有价格字段的小数位数。
		std::string m_strSource;               // 行情数据来源。
		bool m_bStale{ false };                 // 行情是否已过期。
	};

	// 单个标的的买卖盘口深度。
	struct CDepth
	{
		CSecurity m_security;              // 标的信息。
		std::int64_t m_nExchangeTime{ 0 }; // 交易所生成行情的毫秒时间戳。
		std::int64_t m_nReceiveTime{ 0 };  // 服务接收行情的毫秒时间戳。
		std::vector<CPriceLevel> m_bids;   // 买盘档位，通常从买一开始排列。
		std::vector<CPriceLevel> m_asks;   // 卖盘档位，通常从卖一开始排列。
		std::string m_strSource;           // 行情数据来源。
		bool m_bStale{ false };            // 盘口数据是否已过期。
	};

	// 一个时间周期内的K线数据。
	struct CBar
	{
		CSecurity m_security;					 // 标的信息。
		Channel m_channel{ Channel::unknown };   // K线周期，例如一分钟或一天。
		std::int64_t m_nBeginTime{ 0 };          // K线周期开始的毫秒时间戳。
		std::int64_t m_nOpenPrice{ 0 };          // 开盘价。
		std::int64_t m_nHighPrice{ 0 };          // 最高价。
		std::int64_t m_nLowPrice{ 0 };           // 最低价。
		std::int64_t m_nClosePrice{ 0 };         // 收盘价。
		std::int64_t m_nVolume{ 0 };             // 周期内成交量，具体单位由数据源约定。
		std::int64_t m_nTurnover{ 0 };           // 周期内成交额，具体单位由数据源约定。
		int m_nPriceScale{ 4 };                   // 所有价格字段的小数位数。
		std::string m_strAdjustment{ "none" };  // 复权方式，none表示不复权。
		std::string m_strSource;                 // 行情数据来源。
	};

	// 客户端对某个标的、某类行情数据的订阅项。
	struct CChannelInfo
	{
		CSecurity m_security;                 // 订阅的标的。
		Channel m_channel{ Channel::unknown }; // 订阅的数据通道
		std::string String() const;
	};

	// CSecurity的哈希计算器，用于unordered_map等哈希容器。
	struct CSecurityHash
	{
		std::size_t operator()(const CSecurity& instrument) const;
	};
} // namespace market
#endif
