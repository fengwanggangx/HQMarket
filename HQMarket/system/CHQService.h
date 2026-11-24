#ifndef HQMARKET_SYSTEM_CHQSERVICE_H
#define HQMARKET_SYSTEM_CHQSERVICE_H

#include "../quote/CHQBroker.h"
#include "../request/request.h"
#include "CSubscriptionMgr.h"
#include <mutex>
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

class CPythonRuntime;

class CHQService final
{
	friend class CHQRoutingTests;

public:
	using _TyQuoteHandler = CHQBroker::_TyQuoteHandler;
	using _TyDepthHandler = CHQBroker::_TyDepthHandler;
	using _TyBroadcastHandler = std::function<void(const std::vector<net::_TyConnectionId>&, const CRequest&)>;
	using _TyReplyHandler = std::function<bool(net::_TyConnectionId, const CRequest&)>;

	explicit CHQService(CPythonRuntime* pPythonRuntime);
	~CHQService();
	CHQService(const CHQService& arg) = delete;
	CHQService& operator=(const CHQService& arg) = delete;

	void Stop();
	bool Initialize(const std::filesystem::path& root);
	bool OnRequest(const CRequest& req);
	void HandleClientDisconnected(net::_TyConnectionId id);

	void OnDepth(market::CDepth&& depth);
	void OnQuote(const market::CQuote& quote, std::uint64_t nSequence);

	void SetReplyHandler(_TyReplyHandler&& handler);
	void SetBroadcastHandler(_TyBroadcastHandler&& handler);
	void SetQuoteHandler(_TyQuoteHandler&& handler);
	void SetDepthHandler(_TyDepthHandler&& handler);

	std::string HealthJson() const;
	std::string InstrumentsJson() const;
	std::string MetricsText(std::size_t nClients) const;
	std::string QuoteJson(const std::string& strInstrument) const;
	std::string BarsJson(const std::string& strInstrument, const std::string& strChannel, const std::string& strBeginTime, const std::string& strEndTime, int& status, std::optional<std::pair<std::int64_t, std::int64_t>>& returnedRange);

private:
	bool HandleQuery(net::_TyConnectionId id, const CRequest& req);
	bool HandleSubscription(net::_TyConnectionId id, const CRequest& req);
	bool SendResponse(net::_TyConnectionId id, const CRequest& req) const;
	void SendError(net::_TyConnectionId id, const CRequest& req, int code, const std::string& message) const;

	CHQBroker m_broker;
	// Sending can synchronously report a disconnect back to this service.
	std::recursive_mutex m_mtx_subscriptions;
	CSubscriptionMgr m_subscriptions;
	std::atomic_uint64_t m_depth_sequence{ 0 };
	_TyReplyHandler m_handler_reply;
	_TyBroadcastHandler m_broadcastHandler;
	CPythonRuntime* m_python_runtime{ nullptr }; // Borrowed from BootLoader; providers stop before Python finalizes.
};

#endif
