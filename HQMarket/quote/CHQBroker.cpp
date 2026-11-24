#include "CHQBroker.h"
#include "../log/CLogger.h"
#include <map>
#include <utility>

CHQBroker::~CHQBroker()
{
	Stop();
}

bool CHQBroker::Initialize(const std::filesystem::path& root)
{
	if (!m_recorder.Open(root / "data" / "hqmarket.db"))
	{
		return false;
	}
	m_mootdx.SetQuoteHandler([this](market::CQuote&& quote)
							 {
				market::CSecurity security = quote.m_security;
				std::uint64_t sequence = m_cache.Update(std::move(quote));
				std::optional<market::CQuote> cached = m_cache.GetQuote(security);
				if (cached.has_value() && m_quote_handler)
				{
					m_quote_handler(*cached, sequence);
				} });
	m_mootdx.SetDepthHandler([this](market::CDepth&& depth)
	{
		if (m_depth_handler)
		{
			m_depth_handler(std::move(depth));
		}
	});
	if (!m_mootdx.Initialize())
	{
		return false;
	}
	m_akshare.Initialize();
	return true;
}

void CHQBroker::Stop()
{
	m_mootdx.Stop();
	m_akshare.Stop();
	std::lock_guard<std::mutex> lock(m_mtx_history);
	m_historyRefreshes.clear();
	m_recorder.Close();
}

bool CHQBroker::Subscribe(const std::vector<market::CChannelInfo>& infos)
{
	return m_mootdx.Subscribe(infos);
}

bool CHQBroker::Unsubscribe(const std::vector<market::CChannelInfo>& infos)
{
	return m_mootdx.Unsubscribe(infos);
}

std::optional<market::CQuote> CHQBroker::QueryQuote(const market::CSecurity& security) const
{
	return m_cache.GetQuote(security);
}

std::vector<market::CBar> CHQBroker::QueryBars(const market::CSecurity& security, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime)
{
	std::lock_guard<std::mutex> lock(m_mtx_history);
	std::vector<market::CBar> bars = m_recorder.QueryBars(security, channel, nBeginTime, nEndTime);
	std::string key = security.String() + ":" + std::to_string(static_cast<int>(channel)) + ":" +
		std::to_string(nBeginTime) + ":" + std::to_string(nEndTime);
	auto refreshed = m_historyRefreshes.find(key);
	if (!bars.empty() && m_historyRefreshes.end() != refreshed && std::chrono::steady_clock::now() < refreshed->second)
	{
		return bars;
	}
	std::vector<market::CBar> providerBars = m_akshare.QueryBars(security, channel, nBeginTime, nEndTime);
	if (!providerBars.empty())
	{
		if (!m_recorder.UpsertBars(providerBars))
		{
			CLogger::Write(Level::Error, "history", "Failed to persist bars for ", security.String());
		}
		else
		{
			bool primary = true;
			for (const market::CBar& bar : providerBars)
			{
				primary = primary && ("akshare" == bar.m_source);
			}
			if (primary)
			{
				if (1024 <= m_historyRefreshes.size())
				{
					m_historyRefreshes.clear();
				}
				std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::system_clock::now().time_since_epoch()).count();
				auto ttl = nEndTime < nowMs - 86400000 ? std::chrono::seconds(300) : std::chrono::seconds(5);
				m_historyRefreshes.insert_or_assign(std::move(key), std::chrono::steady_clock::now() + ttl);
			}
		}
		std::map<std::int64_t, market::CBar> mergedBars;
		for (const market::CBar& bar : bars)
		{
			mergedBars.insert_or_assign(bar.m_begin_time, bar);
		}
		for (const market::CBar& bar : providerBars)
		{
			mergedBars.insert_or_assign(bar.m_begin_time, bar);
		}
		bars.clear();
		bars.reserve(mergedBars.size());
		for (const auto& value : mergedBars)
		{
			bars.emplace_back(value.second);
		}
	}
	return bars;
}

std::vector<market::CInstrument> CHQBroker::QueryInstruments() const
{
	return m_akshare.QueryInstruments();
}

std::vector<market::CSector> CHQBroker::QuerySectors(market::SectorType type)
{
	return m_akshare.QuerySectors(type);
}

market::CSectorConstituents CHQBroker::QuerySectorConstituents(market::SectorType type, const std::string& strSectorCode)
{
	return m_akshare.QuerySectorConstituents(type, strSectorCode);
}

market::CProviderStatus CHQBroker::RealtimeStatus() const
{
	return m_mootdx.GetStatus();
}

market::CProviderStatus CHQBroker::HistoryStatus() const
{
	return m_akshare.GetStatus();
}

bool CHQBroker::IsRecorderOpen() const
{
	return m_recorder.IsOpen();
}

std::size_t CHQBroker::QuoteCount() const
{
	return m_cache.QuoteCount();
}

void CHQBroker::SetQuoteHandler(_TyQuoteHandler handler)
{
	m_quote_handler = std::move(handler);
}

void CHQBroker::SetDepthHandler(_TyDepthHandler handler)
{
	m_depth_handler = std::move(handler);
}
