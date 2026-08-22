#ifndef __CMARKET_CACHE_H__
#define __CMARKET_CACHE_H__
#include "MarketTypes.h"
#include <optional>
#include <shared_mutex>
#include <unordered_map>
namespace market
{
	class CMarketCache final
	{
		public:
			std::uint64_t Update(CQuote quote);
			std::optional<CQuote> GetQuote(const CInstrument& instrument) const;
			std::size_t QuoteCount() const;

		private:
			mutable std::shared_mutex m_mtxQuotes;
			std::unordered_map<CInstrument, CQuote, CInstrumentHash> m_quotes;
			std::uint64_t m_nQuoteSequence{0};
	};
} // namespace market
#endif
