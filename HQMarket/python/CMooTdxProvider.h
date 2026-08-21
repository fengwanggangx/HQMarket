#ifndef __CMOOTDX_PROVIDER_H__
#define __CMOOTDX_PROVIDER_H__
#include "../market/IMarketProvider.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
namespace provider
{
	class CMooTdxProvider final : public market::IMarketProvider
	{
	public:
		~CMooTdxProvider() override;
		const char* Name() const override;
		bool Initialize() override;
		bool Subscribe(const std::vector<market::CSubscription>& subscriptions) override;
		bool Unsubscribe(const std::vector<market::CSubscription>& subscriptions) override;
		std::vector<market::CBar> QueryBars(const market::CInstrument&, market::Channel, std::int64_t, std::int64_t) override;
		market::CProviderStatus GetStatus() const override;
		void SetQuoteHandler(_TyQuoteHandler handler) override;
		void SetDepthHandler(_TyDepthHandler handler) override;
		void Stop() override;
	private:
		void Run();
		bool Poll(const std::vector<market::CInstrument>& instruments);
		static std::string MakeKey(const market::CSubscription& subscription);
	private:
		std::atomic_bool m_bRunning{ false };
		std::thread m_thread;
		mutable std::mutex m_mtxState;
		std::unordered_map<std::string, market::CSubscription> m_subscriptions;
		_TyQuoteHandler m_quoteHandler;
		_TyDepthHandler m_depthHandler;
		market::CProviderStatus m_status;
		void* m_pProvider{ nullptr };
	};
}
#endif
