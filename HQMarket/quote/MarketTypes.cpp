#include "MarketTypes.h"
#include <functional>
namespace market
{
	std::string GetMarketString(Exchange mk)
	{
		switch (mk)
		{
		case Exchange::sse:
			return "SSE";
		case Exchange::szse:
			return "SZSE";
		case Exchange::bse:
			return "BSE";
		case Exchange::hkex:
			return "HKEX";
		case Exchange::cffex:
			return "CFFEX";
		case Exchange::shfe:
			return "SHFE";
		case Exchange::dce:
			return "DCE";
		case Exchange::czce:
			return "CZCE";
		case Exchange::ine:
			return "INE";
		case Exchange::gfex:
			return "GFEX";
		case Exchange::nasdaq:
			return "NASDAQ";
		case Exchange::nyse:
			return "NYSE";
		case Exchange::crypto:
			return "CRYPTO";
		default:
			return "";
		}
	}

	std::string FmtSecurityString(const std::string& strCode, Exchange mk)
	{
		std::string strMarket = GetMarketString(mk);
		if (strCode.empty() || strMarket.empty())
		{
			return {};
		}
		return strCode + "." + strMarket;
	}

	CSecurity::CSecurity(const CSecurity& arg) : m_code(arg.m_code), m_market(arg.m_market)
	{
	}

	CSecurity::CSecurity(const std::string& strCode, market::Exchange mk) : m_code(strCode), m_market(mk)
	{
	}

	CSecurity& CSecurity::operator=(const CSecurity& arg)
	{
		if (&arg != this)
		{
			m_code = arg.m_code;
			m_market = arg.m_market;
		}
		return *this;
	}

	bool CSecurity::IsValid() const
	{
		return !m_code.empty() && (Exchange::unknown != m_market);
	}

	std::string CSecurity::String() const
	{
		return FmtSecurityString(m_code, m_market);
	}

	bool CSecurity::operator==(const CSecurity& arg) const
	{
		return (m_market == arg.m_market) && (m_code == arg.m_code);
	}

	std::size_t CSecurityHash::operator()(const CSecurity& instrument) const
	{
		return std::hash<std::string>{}(instrument.String());
	}

	std::string CChannelInfo::String() const
	{
		if (!m_security.IsValid())
		{
			return {};
		}
		return m_security.String() + "." + std::to_string(static_cast<int>(m_channel));
	}

} // namespace market
