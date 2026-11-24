#ifndef __CMARKET_CACHE_H__
#define __CMARKET_CACHE_H__
#include "MarketTypes.h"
#include <optional>
#include <utility>
#include <shared_mutex>
#include <unordered_map>

class CHQCache final
{
public:
	std::uint64_t Update(const market::CQuote& quote);
	std::uint64_t Update(market::CQuote&& quote);
	std::optional<market::CQuote> GetQuote(const market::CSecurity& security, std::uint64_t* sequence = nullptr) const;
	std::size_t QuoteCount() const;

private:
	mutable std::shared_mutex m_quotes_mutex;
	std::unordered_map<market::CSecurity, std::pair<market::CQuote, std::uint64_t>, market::CSecurityHash> m_quotes;
	std::uint64_t m_quote_sequence{ 0 };
};
#endif
