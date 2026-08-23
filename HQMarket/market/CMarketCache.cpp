#include "CMarketCache.h"
#include <mutex>
namespace market
{
	std::uint64_t CMarketCache::Update(const CQuote& quote)
	{
		std::unique_lock<std::shared_mutex> lock(m_smtx_quotes);
		m_quotes.insert_or_assign(quote.m_instrument, quote);
		return ++m_nQuoteSequence;
	}

	std::uint64_t CMarketCache::Update(CQuote&& quote)
	{
		std::unique_lock<std::shared_mutex> lock(m_smtx_quotes);
		m_quotes.insert_or_assign(quote.m_instrument, std::move(quote));
		return ++m_nQuoteSequence;
	}
	std::optional<CQuote> CMarketCache::GetQuote(const CInstrument& instrument) const
	{
		std::shared_lock<std::shared_mutex> lock(m_smtx_quotes);
		std::unordered_map<CInstrument, CQuote, CInstrumentHash>::const_iterator iter = m_quotes.find(instrument);
		return iter == m_quotes.end() ? std::nullopt : std::optional<CQuote>(iter->second);
	}
	std::size_t CMarketCache::QuoteCount() const
	{
		std::shared_lock<std::shared_mutex> lock(m_smtx_quotes);
		return m_quotes.size();
	}
} // namespace market
