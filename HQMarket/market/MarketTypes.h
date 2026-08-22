#ifndef __MARKET_TYPES_H__
#define __MARKET_TYPES_H__
#include <cstdint>
#include <string>
#include <vector>
namespace market
{
	enum class Exchange
	{
		unknown = 0,
		sse,
		szse,
		bse,
		hkex,
		cffex,
		shfe,
		dce,
		czce,
		ine,
		gfex,
		nasdaq,
		nyse,
		crypto
	};
	enum class Channel
	{
		unknown = 0,
		quote,
		depth,
		trade,
		bar_1m,
		bar_1d,
		market_status
	};
	struct CInstrument
	{
			std::string m_strSymbol;
			Exchange m_exchange{Exchange::unknown};
			std::string Key() const;
			bool operator==(const CInstrument& other) const;
	};
	struct CPriceLevel
	{
			std::int64_t m_nPrice{0};
			std::int64_t m_nVolume{0};
			int m_nPriceScale{4};
	};
	struct CQuote
	{
			CInstrument m_instrument;
			std::int64_t m_nExchangeTime{0};
			std::int64_t m_nReceiveTime{0};
			std::int64_t m_nLastPrice{0};
			std::int64_t m_nOpenPrice{0};
			std::int64_t m_nHighPrice{0};
			std::int64_t m_nLowPrice{0};
			std::int64_t m_nPreClose{0};
			std::int64_t m_nVolume{0};
			std::int64_t m_nTurnover{0};
			int m_nPriceScale{4};
			std::string m_strSource;
			bool m_bStale{false};
	};
	struct CDepth
	{
			CInstrument m_instrument;
			std::int64_t m_nExchangeTime{0};
			std::int64_t m_nReceiveTime{0};
			std::vector<CPriceLevel> m_bids;
			std::vector<CPriceLevel> m_asks;
			std::string m_strSource;
			bool m_bStale{false};
	};
	struct CBar
	{
			CInstrument m_instrument;
			Channel m_channel{Channel::unknown};
			std::int64_t m_nBeginTime{0};
			std::int64_t m_nOpenPrice{0};
			std::int64_t m_nHighPrice{0};
			std::int64_t m_nLowPrice{0};
			std::int64_t m_nClosePrice{0};
			std::int64_t m_nVolume{0};
			std::int64_t m_nTurnover{0};
			int m_nPriceScale{4};
			std::string m_strAdjustment{"none"};
			std::string m_strSource;
	};
	struct CSubscription
	{
			CInstrument m_instrument;
			Channel m_channel{Channel::unknown};
	};
	struct CInstrumentHash
	{
			std::size_t operator()(const CInstrument& instrument) const;
	};
} // namespace market
#endif
