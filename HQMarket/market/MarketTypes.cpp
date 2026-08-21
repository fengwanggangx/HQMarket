#include "MarketTypes.h"
#include <functional>
namespace market
{
	static const char* GetExchangeName(Exchange exchange)
	{
		switch (exchange) { case Exchange::sse: return "SSE"; case Exchange::szse: return "SZSE"; case Exchange::bse: return "BSE"; case Exchange::hkex: return "HKEX"; case Exchange::cffex: return "CFFEX"; case Exchange::shfe: return "SHFE"; case Exchange::dce: return "DCE"; case Exchange::czce: return "CZCE"; case Exchange::ine: return "INE"; case Exchange::gfex: return "GFEX"; case Exchange::nasdaq: return "NASDAQ"; case Exchange::nyse: return "NYSE"; case Exchange::crypto: return "CRYPTO"; default: return "UNKNOWN"; }
	}
	std::string CInstrument::Key() const { return m_strSymbol + "." + GetExchangeName(m_exchange); }
	bool CInstrument::operator==(const CInstrument& other) const { return (m_exchange == other.m_exchange) && (m_strSymbol == other.m_strSymbol); }
	std::size_t CInstrumentHash::operator()(const CInstrument& instrument) const { return std::hash<std::string>{}(instrument.Key()); }
}
