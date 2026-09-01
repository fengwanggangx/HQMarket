#include "CHQBroker.h"
#include <utility>

namespace market
{
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
		m_mootdx.SetQuoteHandler([this](CQuote&& quote)
			{
				CSecurity security = quote.m_security;
				std::uint64_t sequence = m_cache.Update(std::move(quote));
				std::optional<CQuote> cached = m_cache.GetQuote(security);
				if (cached.has_value() && m_quoteHandler)
				{
					m_quoteHandler(*cached, sequence);
				}
			});
		m_mootdx.SetDepthHandler([this](CDepth&& depth)
			{
				if (m_depthHandler)
				{
					m_depthHandler(std::move(depth));
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
		m_recorder.Close();
	}

	bool CHQBroker::Subscribe(const std::vector<CSubscription>& subscriptions)
	{
		return m_mootdx.Subscribe(subscriptions);
	}

	bool CHQBroker::Unsubscribe(const std::vector<CSubscription>& subscriptions)
	{
		return m_mootdx.Unsubscribe(subscriptions);
	}

	std::optional<CQuote> CHQBroker::QueryQuote(const CSecurity& security) const
	{
		return m_cache.GetQuote(security);
	}

	std::vector<CBar> CHQBroker::QueryBars(const CSecurity& security, Channel channel, std::int64_t nBeginTime,
										  std::int64_t nEndTime)
	{
		std::vector<CBar> bars = m_recorder.QueryBars(security, channel, nBeginTime, nEndTime);
		if (bars.empty())
		{
			bars = m_akshare.QueryBars(security, channel, nBeginTime, nEndTime);
			if (!bars.empty())
			{
				m_recorder.UpsertBars(bars);
			}
		}
		return bars;
	}

	std::vector<CSecurity> CHQBroker::QueryInstruments() const
	{
		return m_akshare.QueryInstruments();
	}

	CProviderStatus CHQBroker::RealtimeStatus() const
	{
		return m_mootdx.GetStatus();
	}

	CProviderStatus CHQBroker::HistoryStatus() const
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
		m_quoteHandler = std::move(handler);
	}

	void CHQBroker::SetDepthHandler(_TyDepthHandler handler)
	{
		m_depthHandler = std::move(handler);
	}
} // namespace market
