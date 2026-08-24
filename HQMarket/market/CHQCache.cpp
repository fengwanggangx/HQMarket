#include "CHQCache.h"
#include <mutex>
namespace market
{
	std::uint64_t CHQCache::Update(const CQuote& quote)
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_quotes);
		m_quotes.insert_or_assign(quote.m_instrument, quote);
		return ++m_nQuoteSequence;
	}

	std::uint64_t CHQCache::Update(CQuote&& quote)
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_quotes);
		m_quotes.insert_or_assign(quote.m_instrument, std::move(quote));
		return ++m_nQuoteSequence;
	}
	std::optional<CQuote> CHQCache::GetQuote(const CInstrument& instrument) const
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_quotes);
		std::unordered_map<CInstrument, CQuote, CInstrumentHash>::const_iterator iter = m_quotes.find(instrument);
		return iter == m_quotes.end() ? std::nullopt : std::optional<CQuote>(iter->second);
	}
	std::size_t CHQCache::QuoteCount() const
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_quotes);
		return m_quotes.size();
	}
} // namespace market
