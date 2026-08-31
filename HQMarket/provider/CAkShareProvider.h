#ifndef __CAKSHARE_PROVIDER_H__
#define __CAKSHARE_PROVIDER_H__
#include "../quote/IMarketProvider.h"
#include <mutex>
namespace provider
{
	class CAkShareProvider final : public market::IMarketProvider
	{
		public:
			~CAkShareProvider() override;
			const char* Name() const override;
			bool Initialize() override;
			bool Subscribe(const std::vector<market::CSubscription>&) override;
			bool Unsubscribe(const std::vector<market::CSubscription>&) override;
			std::vector<market::CBar> QueryBars(const market::CInstrument& instrument, market::Channel channel,
												std::int64_t nBeginTime, std::int64_t nEndTime) override;
			market::CProviderStatus GetStatus() const override;
			void SetQuoteHandler(_TyQuoteHandler) override;
			void SetDepthHandler(_TyDepthHandler) override;
			void Stop() override;
			std::vector<market::CInstrument> QueryInstruments() const;

		private:
			mutable std::mutex m_mtx_state;
			market::CProviderStatus m_status;
			void* m_pProvider{nullptr};
			std::vector<market::CInstrument> m_instruments;
	};
} // namespace provider
#endif
