#ifndef HQMARKET_SYSTEM_CHQCENTER_H
#define HQMARKET_SYSTEM_CHQCENTER_H

#include "../network/common_net.h"
#include "../request/request.h"
#include "../quote/MarketTypes.h"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

class CHQService;
namespace net
{
	class CHttpRequest;
	struct CHttpResponseData;
} // namespace net

class CHQCenter final
{
	friend class CHQRoutingTests;

public:
	explicit CHQCenter(CHQService* pHQService);
	~CHQCenter();
	CHQCenter(const CHQCenter& arg) = delete;
	CHQCenter& operator=(const CHQCenter& arg) = delete;

	void Stop();
	bool Initialize();
	void OnRequest(const CRequest& req);
	int OnClientNetEvent(const net::CNetEvent& ev);

	std::unique_ptr<net::CHttpResponseData> OnBarsRequest(const net::CHttpRequest& req);
	std::unique_ptr<net::CHttpResponseData> OnQuoteRequest(const net::CHttpRequest& req);
	std::unique_ptr<net::CHttpResponseData> OnHealthRequest(const net::CHttpRequest& req);
	std::unique_ptr<net::CHttpResponseData> OnMetricsRequest(const net::CHttpRequest& req);
	std::unique_ptr<net::CHttpResponseData> OnInstrumentsRequest(const net::CHttpRequest& req);

private:
	void OnDepth(market::CDepth&& depth);
	void OnQuote(const market::CQuote& quote, std::uint64_t nSequence);
	bool SendRequest(net::_TyConnectionId id, const CRequest& req);

	bool IsAuthenticated(net::_TyConnectionId id) const;
	void HandleClientDisconnected(net::_TyConnectionId id);
	bool HandleAuth(net::_TyConnectionId id, const CRequest& req);
	bool HandleReAuth(net::_TyConnectionId id, const CRequest& req);
	bool HandleHeartbeat(net::_TyConnectionId id, const CRequest& req);
	bool HandleMarketRequest(net::_TyConnectionId id, const CRequest& req);
	bool Login(net::_TyConnectionId id, const CRequest& req, std::string& strToken);
	void SendAuthResponse(net::_TyConnectionId id, const CRequest& req, int nErrorCode, const std::string& strMessage) const;

	using _TyHandler = std::function<bool(net::_TyConnectionId, const CRequest&)>;
	std::unordered_map<_TyRequest, _TyHandler> m_handler;
	mutable std::mutex m_mtx_sessions;
	std::unordered_set<net::_TyConnectionId> m_auth_clients;
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_client_tokens;
	CHQService* m_pHQService{ nullptr }; // Borrowed from BootLoader; callback producers stop first.
	bool m_bInitialized{ false };
};

#endif
