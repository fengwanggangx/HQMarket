#ifndef HQMARKET_SYSTEM_CHQSERVICE_H
#define HQMARKET_SYSTEM_CHQSERVICE_H

#include "../quote/CHQBroker.h"
#include "../request/request.h"
#include "CSubscriptionMgr.h"
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
	void SetQuoteHandler(_TyQuoteHandler&& handler);
	void SetDepthHandler(_TyDepthHandler&& handler);

	std::string HealthJson() const;
	std::string InstrumentsJson() const;
	std::string MetricsText(std::size_t nClients) const;
	std::string QuoteJson(const std::string& strInstrument) const;
	std::string BarsJson(const std::string& strInstrument, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime);

private:
	bool HandleQuery(net::_TyConnectionId id, const CRequest& req);
	bool HandleSubscription(net::_TyConnectionId id, const CRequest& req);
	bool SendResponse(net::_TyConnectionId id, const CRequest& req) const;
	void SendError(net::_TyConnectionId id, const CRequest& req, int code, const std::string& message) const;

	CHQBroker m_broker;
	CSubscriptionMgr m_subscriptions;
	std::atomic_uint64_t m_nDepthSequence{ 0 };
	_TyReplyHandler m_handler_reply;
	CPythonRuntime* m_pPythonRuntime{ nullptr }; // Borrowed from BootLoader; providers stop before Python finalizes.
};

#endif
