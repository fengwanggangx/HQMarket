#include "CTcpServer.h"
#include "netcommon.h"
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <utility>

namespace net
{
	struct CTcpServer::CConnection
	{
		CTcpServer* m_pOwner{nullptr};
		ConnectionId m_connectionId{0};
		struct bufferevent* m_pEvent{nullptr};
	};

	CTcpServer::CTcpServer(int nPort) : m_nPort(nPort)
	{
	}

	CTcpServer::~CTcpServer()
	{
		Release();
	}

	int CTcpServer::Initialize()
	{
		if ((GetNet() == nullptr) || (m_pListener != nullptr))
		{
			return -1;
		}
		struct sockaddr_in address;
		if (FmtAddress(address, m_nPort) == false)
		{
			return -1;
		}
		m_pListener = evconnlistener_new_bind(GetNet(), AcceptCallback, this,
											 LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
											 reinterpret_cast<sockaddr*>(&address), sizeof(address));
		return (m_pListener != nullptr) ? 0 : -1;
	}

	void CTcpServer::RegisterDataHandler(DataHandler handler)
	{
		m_dataHandler = std::move(handler);
	}

	void CTcpServer::RegisterConnectedHandler(ConnectionHandler handler)
	{
		m_connectedHandler = std::move(handler);
	}

	void CTcpServer::RegisterDisconnectedHandler(ConnectionHandler handler)
	{
		m_disconnectedHandler = std::move(handler);
	}

	void CTcpServer::AcceptCallback(evconnlistener*, evutil_socket_t fd, sockaddr* pAddress, int nLength, void* pContext)
	{
		CTcpServer* pServer = static_cast<CTcpServer*>(pContext);
		if (pServer != nullptr)
		{
			pServer->OnAccept(fd, pAddress, nLength);
		}
	}

	void CTcpServer::ReadCallback(bufferevent*, void* pContext)
	{
		CConnection* pConnection = static_cast<CConnection*>(pContext);
		if ((pConnection != nullptr) && (pConnection->m_pOwner != nullptr))
		{
			pConnection->m_pOwner->OnRead(*pConnection);
		}
	}

	void CTcpServer::EventCallback(bufferevent*, short nEvents, void* pContext)
	{
		CConnection* pConnection = static_cast<CConnection*>(pContext);
		if ((pConnection != nullptr) && (pConnection->m_pOwner != nullptr))
		{
			pConnection->m_pOwner->OnEvent(*pConnection, nEvents);
		}
	}

	void CTcpServer::OnAccept(evutil_socket_t fd, sockaddr*, int)
	{
		std::unique_ptr<CConnection> connection = std::make_unique<CConnection>();
		connection->m_pOwner = this;
		connection->m_connectionId = m_nextConnectionId.fetch_add(1);
		int nOptions = BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS;
		if (IsThreadEnable() == true)
		{
			nOptions |= BEV_OPT_THREADSAFE;
		}
		connection->m_pEvent = bufferevent_socket_new(GetNet(), fd, nOptions);
		if (connection->m_pEvent == nullptr)
		{
			evutil_closesocket(fd);
			return;
		}
		bufferevent_setcb(connection->m_pEvent, ReadCallback, nullptr, EventCallback, connection.get());
		bufferevent_enable(connection->m_pEvent, EV_READ | EV_WRITE);
		ConnectionId connectionId = connection->m_connectionId;
		{
			std::lock_guard<std::mutex> lock(m_mtxConnections);
			m_connections.emplace(connectionId, std::move(connection));
		}
		if (m_connectedHandler != nullptr)
		{
			m_connectedHandler(connectionId);
		}
	}

	void CTcpServer::OnRead(CConnection& connection)
	{
		struct evbuffer* pInput = bufferevent_get_input(connection.m_pEvent);
		if (pInput == nullptr)
		{
			return;
		}
		std::size_t nLength = evbuffer_get_length(pInput);
		if (nLength == 0)
		{
			return;
		}
		std::vector<std::uint8_t> data(nLength);
		int nRemoved = evbuffer_remove(pInput, data.data(), data.size());
		if (nRemoved <= 0)
		{
			return;
		}
		data.resize(static_cast<std::size_t>(nRemoved));
		if (m_dataHandler != nullptr)
		{
			m_dataHandler(connection.m_connectionId, std::move(data));
		}
	}

	void CTcpServer::OnEvent(CConnection& connection, short nEvents)
	{
		if ((nEvents & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) != 0)
		{
			Close(connection.m_connectionId);
		}
	}

	bool CTcpServer::Send(ConnectionId connectionId, const void* pData, std::size_t nLength)
	{
		if ((pData == nullptr) || (nLength == 0))
		{
			return false;
		}
		std::lock_guard<std::mutex> lock(m_mtxConnections);
		std::unordered_map<ConnectionId, std::unique_ptr<CConnection>>::iterator connection =
			m_connections.find(connectionId);
		if ((connection == m_connections.end()) || (connection->second->m_pEvent == nullptr))
		{
			return false;
		}
		return bufferevent_write(connection->second->m_pEvent, pData, nLength) == 0;
	}

	bool CTcpServer::Close(ConnectionId connectionId)
	{
		std::unique_ptr<CConnection> connection;
		{
			std::lock_guard<std::mutex> lock(m_mtxConnections);
			std::unordered_map<ConnectionId, std::unique_ptr<CConnection>>::iterator found =
				m_connections.find(connectionId);
			if (found == m_connections.end())
			{
				return false;
			}
			connection = std::move(found->second);
			m_connections.erase(found);
		}
		if (connection->m_pEvent != nullptr)
		{
			bufferevent_free(connection->m_pEvent);
			connection->m_pEvent = nullptr;
		}
		if (m_disconnectedHandler != nullptr)
		{
			m_disconnectedHandler(connectionId);
		}
		return true;
	}

	std::size_t CTcpServer::ClientCount() const
	{
		std::lock_guard<std::mutex> lock(m_mtxConnections);
		return m_connections.size();
	}

	void CTcpServer::Release()
	{
		if (m_pListener != nullptr)
		{
			evconnlistener_free(m_pListener);
			m_pListener = nullptr;
		}
		std::unordered_map<ConnectionId, std::unique_ptr<CConnection>> connections;
		{
			std::lock_guard<std::mutex> lock(m_mtxConnections);
			connections.swap(m_connections);
		}
		for (std::pair<const ConnectionId, std::unique_ptr<CConnection>>& item : connections)
		{
			if (item.second->m_pEvent != nullptr)
			{
				bufferevent_free(item.second->m_pEvent);
				item.second->m_pEvent = nullptr;
			}
		}
	}
} // namespace net
