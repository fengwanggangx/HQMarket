#include "CHQCache.h"
#include <mutex>

std::uint64_t CHQCache::Update(const market::CQuote& quote)
{
	std::unique_lock<std::shared_mutex> lck(m_mtx_quotes);
	m_quotes.insert_or_assign(quote.m_security, quote);
	return ++m_nQuoteSequence;
}

std::uint64_t CHQCache::Update(market::CQuote&& quote)
{
	std::unique_lock<std::shared_mutex> lck(m_mtx_quotes);
	m_quotes.insert_or_assign(quote.m_security, std::move(quote));
	return ++m_nQuoteSequence;
}

std::optional<market::CQuote> CHQCache::GetQuote(const market::CSecurity& security) const
{
	std::shared_lock<std::shared_mutex> lck(m_mtx_quotes);
	std::unordered_map<market::CSecurity, market::CQuote, market::CSecurityHash>::const_iterator mIter = m_quotes.find(security);
	return m_quotes.end() == mIter ? std::nullopt : std::optional<market::CQuote>(mIter->second);
}

std::size_t CHQCache::QuoteCount() const
{
	std::shared_lock<std::shared_mutex> lck(m_mtx_quotes);
	return m_quotes.size();
}
