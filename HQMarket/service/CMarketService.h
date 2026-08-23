#ifndef __CMARKET_SERVICE_H__
#define __CMARKET_SERVICE_H__

#include "../market/CMarketCache.h"
#include "../market/CSubscriptionManager.h"
#include "../market/v1/market.pb.h"
#include "../network/CFrameCodec.h"
#include "../network/CTcpServer.h"
#include "../python/CAkShareProvider.h"
#include "../python/CMooTdxProvider.h"
#include "../python/CPythonRuntime.h"
#include "../storage/CMarketStorage.h"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class CRequest;

namespace service
{
	class CMarketService final
	{
		private:
			struct CClientSession
			{
				bool m_bAuthenticated{false};
			};

		public:
			explicit CMarketService(net::CTcpServer* pTcpServer);
			bool Initialize(const std::string& strToken, const std::filesystem::path& root);
			void Stop();
			std::string HealthJson() const;
			std::string MetricsText() const;
			std::string QuoteJson(const std::string& strInstrument) const;
			std::string InstrumentsJson() const;
			std::string BarsJson(const std::string& strInstrument, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime);

		private:
			int OnClientRequest(const std::unique_ptr<CRequest>& request);
			void OnClientDisconnected(net::_TyConnectionId id);
			void HandleEnvelope(net::_TyConnectionId id, const hqmarket::market::v1::MarketEnvelope& envelope);
			void SendEnvelope(net::_TyConnectionId id, hqmarket::market::v1::MarketEnvelope& envelope);
			void PublishQuote(const market::CQuote& quote, std::uint64_t nSequence);
			void PublishDepth(const market::CDepth& depth, std::uint64_t nSequence);
			bool IsAuthenticated(net::_TyConnectionId id) const;
			std::vector<net::_TyConnectionId> AuthenticatedClients() const;

		private:
			provider::CPythonRuntime m_python;
			provider::CMooTdxProvider m_mootdx;
			provider::CAkShareProvider m_akshare;
			market::CMarketCache m_cache;
			storage::CMarketStorage m_storage;
			net::CTcpServer* m_pTcpServer{nullptr};
			std::string m_strToken;
			mutable std::mutex m_mtx_sessions;
			std::unordered_map<net::_TyConnectionId, CClientSession> m_sessions;
			market::CSubscriptionManager m_subscriptions;
			std::atomic_uint64_t m_nDepthSequence{0};
	};
} // namespace service

#endif
