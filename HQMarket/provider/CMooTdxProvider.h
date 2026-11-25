#ifndef __CMOOTDX_PROVIDER_H__
#define __CMOOTDX_PROVIDER_H__
#include "../quote/IMarketProvider.h"
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
		bool Subscribe(const std::vector<market::CChannelInfo>& subscriptions) override;
		bool Unsubscribe(const std::vector<market::CChannelInfo>& subscriptions) override;
		std::vector<market::CBar> QueryBars(const market::CSecurity&, market::Channel, std::int64_t, std::int64_t) override;
		market::CProviderStatus GetStatus() const override;
		void SetQuoteHandler(_TyQuoteHandler handler) override;
		void SetDepthHandler(_TyDepthHandler handler) override;
		void Stop() override;

	private:
		void Run();
		std::size_t Poll(const std::vector<market::CSecurity>& securities);
		static std::string MakeKey(const market::CChannelInfo& subscription);

	private:
		std::atomic_bool m_running{ false };
		std::thread m_thread;
		mutable std::mutex m_state_mutex;
		std::unordered_map<std::string, market::CChannelInfo> m_subscriptions;
		_TyQuoteHandler m_quote_handler;
		_TyDepthHandler m_depth_handler;
		market::CProviderStatus m_status;
		void* m_provider{ nullptr };
	};
} // namespace provider
#endif
