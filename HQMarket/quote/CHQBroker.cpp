#include "CHQBroker.h"
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
				if (cached.has_value() && m_quoteHandler)
				{
					m_quoteHandler(*cached, sequence);
				}
			});
		m_mootdx.SetDepthHandler([this](market::CDepth&& depth)
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
		std::vector<market::CBar> bars = m_recorder.QueryBars(security, channel, nBeginTime, nEndTime);
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

std::vector<market::CSecurity> CHQBroker::QueryInstruments() const
	{
		return m_akshare.QueryInstruments();
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
		m_quoteHandler = std::move(handler);
	}

void CHQBroker::SetDepthHandler(_TyDepthHandler handler)
	{
		m_depthHandler = std::move(handler);
	}
