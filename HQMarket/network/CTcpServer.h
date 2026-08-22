#ifndef __CTCPSERVER_H__
#define __CTCPSERVER_H__

#include "CNet.h"
#include <event2/util.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

struct bufferevent;
struct evconnlistener;

namespace net
{
	class CTcpServer final : public CNet
	{
		public:
			using ConnectionId = std::uint64_t;
			using DataHandler = std::function<void(ConnectionId, std::vector<std::uint8_t>&&)>;
			using ConnectionHandler = std::function<void(ConnectionId)>;

			explicit CTcpServer(int nPort);
			~CTcpServer() override;

			int Initialize();
			void RegisterDataHandler(DataHandler handler);
			void RegisterConnectedHandler(ConnectionHandler handler);
			void RegisterDisconnectedHandler(ConnectionHandler handler);
			bool Send(ConnectionId connectionId, const void* pData, std::size_t nLength);
			bool Close(ConnectionId connectionId);
			std::size_t ClientCount() const;

		private:
			struct CConnection;
			static void AcceptCallback(struct evconnlistener* pListener, evutil_socket_t fd, struct sockaddr* pAddress,
								   int nLength, void* pContext);
			static void ReadCallback(struct bufferevent* pEvent, void* pContext);
			static void EventCallback(struct bufferevent* pEvent, short nEvents, void* pContext);
			void OnAccept(evutil_socket_t fd, struct sockaddr* pAddress, int nLength);
			void OnRead(CConnection& connection);
			void OnEvent(CConnection& connection, short nEvents);
			void Release();

		private:
			int m_nPort{-1};
			std::atomic_uint64_t m_nextConnectionId{1};
			struct evconnlistener* m_pListener{nullptr};
			mutable std::mutex m_mtxConnections;
			std::unordered_map<ConnectionId, std::unique_ptr<CConnection>> m_connections;
			DataHandler m_dataHandler;
			ConnectionHandler m_connectedHandler;
			ConnectionHandler m_disconnectedHandler;
	};
} // namespace net

#endif
