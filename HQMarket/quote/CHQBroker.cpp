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
	std::lock_guard<std::mutex> fetchLock(m_mtx_historyfetch);
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

std::optional<market::CQuote> CHQBroker::QueryQuote(const market::CSecurity& security, std::uint64_t* sequence) const
{
	return m_cache.GetQuote(security, sequence);
}

std::vector<market::CBar> CHQBroker::QueryBars(const market::CSecurity& security, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime, market::CProviderStatus* queryStatus)
{

	std::string key = security.String() + ":" + std::to_string(static_cast<int>(channel)) + ":" +
		std::to_string(nBeginTime) + ":" + std::to_string(nEndTime);
	if (0 == nEndTime)
	{
		nEndTime = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}
	auto isFresh = [this, &key]()
	{
		std::lock_guard<std::mutex> lock(m_mtx_history);
		auto refreshed = m_historyRefreshes.find(key);
		return m_historyRefreshes.end() != refreshed && std::chrono::steady_clock::now() < refreshed->second;
	};
	if (isFresh())
	{
		return m_recorder.QueryBars(security, channel, nBeginTime, nEndTime);
	}
	std::lock_guard<std::mutex> fetchLock(m_mtx_historyfetch);
	std::vector<market::CBar> bars = m_recorder.QueryBars(security, channel, nBeginTime, nEndTime);
	if (isFresh())
	{
		return bars;
	}
	std::vector<market::CBar> providerBars = m_akshare.QueryBars(security, channel, nBeginTime, nEndTime, queryStatus);
	if (!providerBars.empty())
	{
		std::map<std::int64_t, market::CBar> mergedBars;
		for (const market::CBar& bar : bars)
		{
			mergedBars.insert_or_assign(bar.m_begin_time, bar);
		}
		for (market::CBar& bar : providerBars)
		{
			auto previous = mergedBars.find(bar.m_begin_time);
			if (!bar.m_hasTurnover && mergedBars.end() != previous)
			{
				bar.m_turnover = previous->second.m_turnover;
			}
			mergedBars.insert_or_assign(bar.m_begin_time, bar);
		}
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
				std::lock_guard<std::mutex> lock(m_mtx_history);
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
