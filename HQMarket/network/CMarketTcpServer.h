#ifndef __CMARKET_TCP_SERVER_H__
#define __CMARKET_TCP_SERVER_H__
#include "CFrameCodec.h"
#include "CNet.h"
#include "../market/CSubscriptionManager.h"
#include "../market/v1/market.pb.h"
#include <event2/util.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
struct bufferevent;
struct evconnlistener;
namespace net
{
	class CMarketTcpServer final : public CNet
	{
	public:
		using _TySubscriptionHandler = std::function<void(const std::vector<market::CSubscription>&, bool)>;
		CMarketTcpServer(int nPort, std::string strToken);
		~CMarketTcpServer() override;
		int Initialize();
		void SetSubscriptionHandler(_TySubscriptionHandler handler);
		void PublishQuote(const market::CQuote& quote, std::uint64_t nSequence);
		void PublishDepth(const market::CDepth& depth, std::uint64_t nSequence);
		std::size_t ClientCount() const;
	private:
		struct CConnection;
		static void AcceptCallback(struct evconnlistener* pListener, evutil_socket_t fd, struct sockaddr* pAddress, int nLength, void* pContext);
		static void ReadCallback(struct bufferevent* pEvent, void* pContext);
		static void EventCallback(struct bufferevent* pEvent, short nEvents, void* pContext);
		void OnAccept(evutil_socket_t fd, struct sockaddr* pAddress, int nLength);
		void OnRead(CConnection& connection);
		void OnEvent(CConnection& connection, short nEvents);
		void Handle(CConnection& connection, const hqmarket::market::v1::MarketEnvelope& envelope);
		void Send(CConnection& connection, hqmarket::market::v1::MarketEnvelope& envelope);
		void Close(evutil_socket_t fd);
	private:
		int m_nPort;
		std::string m_strToken;
		struct evconnlistener* m_pListener{ nullptr };
		mutable std::mutex m_mtxConnections;
		std::unordered_map<evutil_socket_t, std::unique_ptr<CConnection>> m_connections;
		market::CSubscriptionManager m_subscriptions;
		_TySubscriptionHandler m_subscriptionHandler;
	};
}
#endif
