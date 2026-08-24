#ifndef __CMARKET_CACHE_H__
#define __CMARKET_CACHE_H__
#include "MarketTypes.h"
#include <optional>
#include <shared_mutex>
#include <unordered_map>

class CHQCache final
{
public:
	std::uint64_t Update(const market::CQuote& quote);
	std::uint64_t Update(market::CQuote&& quote);
	std::optional<market::CQuote> GetQuote(const market::CInstrument& instrument) const;
	std::size_t QuoteCount() const;

private:
	mutable std::shared_mutex m_mtx_quotes;
	std::unordered_map<market::CInstrument, market::CQuote, market::CInstrumentHash> m_quotes;
	std::uint64_t m_nQuoteSequence{ 0 };
};
#endif
