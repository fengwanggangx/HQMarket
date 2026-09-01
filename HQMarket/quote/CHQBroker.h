#ifndef __CHQ_BROKER_H__
#define __CHQ_BROKER_H__

#include "CHQCache.h"
#include "CHQRecorder.h"
#include "../provider/CAkShareProvider.h"
#include "../provider/CMooTdxProvider.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace market
{
	class CHQBroker final
	{
		public:
			using _TyQuoteHandler = std::function<void(const CQuote&, std::uint64_t)>;
			using _TyDepthHandler = std::function<void(CDepth&&)>;

			~CHQBroker();
			bool Initialize(const std::filesystem::path& root);
			void Stop();
			bool Subscribe(const std::vector<CSubscription>& subscriptions);
			bool Unsubscribe(const std::vector<CSubscription>& subscriptions);
			std::optional<CQuote> QueryQuote(const CSecurity& security) const;
			std::vector<CBar> QueryBars(const CSecurity& security, Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime);
			std::vector<CSecurity> QueryInstruments() const;
			CProviderStatus RealtimeStatus() const;
			CProviderStatus HistoryStatus() const;
			bool IsRecorderOpen() const;
			std::size_t QuoteCount() const;
			void SetQuoteHandler(_TyQuoteHandler handler);
			void SetDepthHandler(_TyDepthHandler handler);

		private:
			provider::CMooTdxProvider m_mootdx;
			provider::CAkShareProvider m_akshare;
			CHQCache m_cache;
			CHQRecorder m_recorder;
			_TyQuoteHandler m_quoteHandler;
			_TyDepthHandler m_depthHandler;
	};
} // namespace market

#endif
