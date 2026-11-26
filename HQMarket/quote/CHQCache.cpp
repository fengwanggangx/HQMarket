#include "CHQCache.h"
#include <mutex>

std::uint64_t CHQCache::Update(const market::CQuote& quote)
{
	return Update(market::CQuote{ quote });
}

std::uint64_t CHQCache::Update(market::CQuote&& quote)
{
	std::unique_lock<std::shared_mutex> lck(m_quotes_mutex);
	const auto mIter = m_quotes.find(quote.m_security);
	if (m_quotes.end() != mIter)
	{
		const market::CQuote& current = mIter->second.first;
		bool comparableExchangeTime = 0 < quote.m_exchange_time && 0 < current.m_exchange_time;
		bool older = comparableExchangeTime ? quote.m_exchange_time < current.m_exchange_time
											: 0 < quote.m_receive_time && quote.m_receive_time < current.m_receive_time;
		if (older || (quote.m_stale && !current.m_stale))
		{
			return 0;
		}
	}
	market::CSecurity security = quote.m_security;
	m_quotes.insert_or_assign(std::move(security), std::make_pair(std::move(quote), ++m_quote_sequence));
	return m_quote_sequence;
}

std::optional<market::CQuote> CHQCache::GetQuote(const market::CSecurity& security, std::uint64_t* sequence) const
{
	std::shared_lock<std::shared_mutex> lck(m_quotes_mutex);
	auto entry = m_quotes.find(security);
	if (m_quotes.end() == entry)
	{
		return std::nullopt;
	}
	if (nullptr != sequence)
	{
		*sequence = entry->second.second;
	}
	return entry->second.first;
}

std::size_t CHQCache::QuoteCount() const
{
	std::shared_lock<std::shared_mutex> lck(m_quotes_mutex);
	return m_quotes.size();
}
