#ifndef __IMARKET_PROVIDER_H__
#define __IMARKET_PROVIDER_H__
#include "MarketTypes.h"
#include <functional>
#include <string>
#include <vector>
namespace market
{
	struct CProviderStatus
	{
		bool m_bHealthy{ false };
		std::string m_strDetail;
	};
	class IMarketProvider
	{
		public:
			using _TyQuoteHandler = std::function<void(CQuote&&)>;
			using _TyDepthHandler = std::function<void(CDepth&&)>;
			virtual ~IMarketProvider() = default;
			virtual const char* Name() const = 0;
			virtual bool Initialize() = 0;
			virtual bool Subscribe(const std::vector<CSubscription>& subscriptions) = 0;
			virtual bool Unsubscribe(const std::vector<CSubscription>& subscriptions) = 0;
			virtual std::vector<CBar> QueryBars(const CInstrument& instrument, Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime) = 0;
			virtual CProviderStatus GetStatus() const = 0;
			virtual void SetQuoteHandler(_TyQuoteHandler handler) = 0;
			virtual void SetDepthHandler(_TyDepthHandler handler) = 0;
			virtual void Stop() = 0;
	};
} // namespace market
#endif
