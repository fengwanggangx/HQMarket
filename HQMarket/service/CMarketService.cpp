#include "CMarketService.h"
#include <sstream>
namespace service
{
	bool CMarketService::Initialize(int nTcpPort, const std::string& strToken, const std::filesystem::path& root)
	{
		if (strToken.empty()) return false;
		if (!m_storage.Open(root / "data" / "hqmarket.db")) return false;
		if (!m_python.Initialize(root / "runtime" / "python", root / "python")) return false;
		m_pTcpServer = std::make_unique<net::CMarketTcpServer>(nTcpPort, strToken); if (m_pTcpServer->Initialize() != 0) return false;
		m_mootdx.SetQuoteHandler([this](market::CQuote&& quote) { const auto instrument = quote.m_instrument; const auto sequence = m_cache.Update(std::move(quote)); const auto cached = m_cache.GetQuote(instrument); if (cached) m_pTcpServer->PublishQuote(*cached, sequence); });
		m_mootdx.SetDepthHandler([this](market::CDepth&& depth) { m_pTcpServer->PublishDepth(depth, ++m_nDepthSequence); });
		if (!m_mootdx.Initialize()) return false;
		m_akshare.Initialize();
		m_pTcpServer->SetSubscriptionHandler([this](const std::vector<market::CSubscription>& values, bool subscribe) { if (subscribe) m_mootdx.Subscribe(values); else m_mootdx.Unsubscribe(values); }); return true;
	}
	void CMarketService::Run() { if (m_pTcpServer) m_pTcpServer->Start(true); }
	void CMarketService::Stop() { m_mootdx.Stop(); m_akshare.Stop(); if (m_pTcpServer) m_pTcpServer->ShutDown(); m_python.Finalize(); m_storage.Close(); }
	std::string CMarketService::HealthJson() const { const auto realtime = m_mootdx.GetStatus(); const auto history = m_akshare.GetStatus(); std::ostringstream out; out << "{\"status\":\"" << ((realtime.m_bHealthy && history.m_bHealthy) ? "ok" : "degraded") << "\",\"python\":" << (m_python.IsInitialized() ? "true" : "false") << ",\"sqlite\":" << (m_storage.IsOpen() ? "true" : "false") << ",\"mootdx\":" << (realtime.m_bHealthy ? "true" : "false") << ",\"akshare\":" << (history.m_bHealthy ? "true" : "false") << "}"; return out.str(); }
	std::string CMarketService::MetricsText() const { std::ostringstream out; out << "hqmarket_clients " << (m_pTcpServer ? m_pTcpServer->ClientCount() : 0) << "\nhqmarket_quotes_cached " << m_cache.QuoteCount() << "\n"; return out.str(); }
	static market::CInstrument ParseInstrument(const std::string& value)
	{
		market::CInstrument result; const auto dot = value.rfind('.'); if (dot == std::string::npos) return result; result.m_strSymbol = value.substr(0, dot); const auto exchange = value.substr(dot + 1); if (exchange == "SSE") result.m_exchange = market::Exchange::sse; else if (exchange == "SZSE") result.m_exchange = market::Exchange::szse; else if (exchange == "BSE") result.m_exchange = market::Exchange::bse; else if (exchange == "HKEX") result.m_exchange = market::Exchange::hkex; return result;
	}
	std::string CMarketService::QuoteJson(const std::string& strInstrument) const
	{
		const auto quote = m_cache.GetQuote(ParseInstrument(strInstrument)); if (!quote) return "{}"; std::ostringstream out; out << "{\"instrument\":\"" << quote->m_instrument.Key() << "\",\"exchange_time_ms\":" << quote->m_nExchangeTime << ",\"receive_time_ms\":" << quote->m_nReceiveTime << ",\"last_price\":" << quote->m_nLastPrice << ",\"price_scale\":" << quote->m_nPriceScale << ",\"volume\":" << quote->m_nVolume << ",\"turnover\":" << quote->m_nTurnover << ",\"source\":\"" << quote->m_strSource << "\",\"stale\":" << (quote->m_bStale ? "true" : "false") << "}"; return out.str();
	}
	std::string CMarketService::InstrumentsJson() const
	{
		const auto values = m_akshare.QueryInstruments(); std::ostringstream out; out << '['; bool first = true; for (const auto& instrument : values) { if (!first) out << ','; first = false; out << "{\"instrument\":\"" << instrument.Key() << "\"}"; } out << ']'; return out.str();
	}
	std::string CMarketService::BarsJson(const std::string& strInstrument, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime)
	{
		const auto instrument = ParseInstrument(strInstrument); auto values = m_storage.QueryBars(instrument, channel, nBeginTime, nEndTime); if (values.empty()) { values = m_akshare.QueryBars(instrument, channel, nBeginTime, nEndTime); if (!values.empty()) m_storage.UpsertBars(values); } std::ostringstream out; out << '['; bool first = true; for (const auto& bar : values) { if (!first) out << ','; first = false; out << "{\"begin_time_ms\":" << bar.m_nBeginTime << ",\"open\":" << bar.m_nOpenPrice << ",\"high\":" << bar.m_nHighPrice << ",\"low\":" << bar.m_nLowPrice << ",\"close\":" << bar.m_nClosePrice << ",\"volume\":" << bar.m_nVolume << ",\"price_scale\":" << bar.m_nPriceScale << "}"; } out << ']'; return out.str();
	}
}
