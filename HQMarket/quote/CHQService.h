#ifndef __CMARKET_SERVICE_H__
#define __CMARKET_SERVICE_H__

#include "CHQBroker.h"
#include "CSubscriptionMgr.h"
#include "v1/market.pb.h"
#include "../network/CFrameCodec.h"
#include "../network/CTcpServer.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class CRequest;
class CPythonRuntime;

class CMarketService final
{
	private:
		struct CClientSession
		{
			bool m_bAuthenticated{ false };
		};

	public:
			explicit CMarketService(net::CTcpServer* pTcpServer, CPythonRuntime* pPythonRuntime);
			bool Initialize(const std::string& strToken, const std::filesystem::path& root);
			void Stop();
			std::string HealthJson() const;
			std::string MetricsText() const;
			std::string QuoteJson(const std::string& strInstrument) const;
			std::string InstrumentsJson() const;
			std::string BarsJson(const std::string& strInstrument, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime);

	private:
			int OnNetEvent(const net::CNetEvent& netEvent);
			int OnClientRequest(const std::unique_ptr<CRequest>& request);
			void OnClientDisconnected(net::_TyConnectionId id);
			void HandleRequest(net::_TyConnectionId id, CRequest& request);
			bool HandleAuth(net::_TyConnectionId id, CRequest& request);
			bool HandleHeartbeat(net::_TyConnectionId id, CRequest& request);
			bool HandleQuery(net::_TyConnectionId id, CRequest& request);
			bool HandleSubscription(net::_TyConnectionId id, CRequest& request);
			void SendRequest(net::_TyConnectionId id, CRequest& request);
			void PublishQuote(const market::CQuote& quote, std::uint64_t nSequence);
			void PublishDepth(const market::CDepth& depth, std::uint64_t nSequence);
			bool IsAuthenticated(net::_TyConnectionId id) const;
			std::vector<net::_TyConnectionId> AuthenticatedClients() const;

	private:
			std::string m_strToken;
			std::unordered_map<std::string, std::function<bool(net::_TyConnectionId, CRequest&)>> m_handler;

			mutable std::mutex m_mtx_sessions;
			std::unordered_map<net::_TyConnectionId, CClientSession> m_sessions;

			market::CSubscriptionMgr m_subscriptions;
			std::atomic_uint64_t m_nDepthSequence{ 0 };

	private:
			CHQBroker m_broker;
			net::CTcpServer* m_pTcpServer{ nullptr };
			CPythonRuntime* m_pPythonRuntime{ nullptr };
};

#endif
