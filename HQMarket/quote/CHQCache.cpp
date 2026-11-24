#include "CHQCache.h"
#include <mutex>

std::uint64_t CHQCache::Update(const market::CQuote& quote)
{
	std::unique_lock<std::shared_mutex> lck(m_quotes_mutex);
	m_quotes.insert_or_assign(quote.m_security, std::make_pair(quote, ++m_quote_sequence));
	return m_quote_sequence;
}

std::uint64_t CHQCache::Update(market::CQuote&& quote)
{
	std::unique_lock<std::shared_mutex> lck(m_quotes_mutex);
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
