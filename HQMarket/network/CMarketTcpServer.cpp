#include "CMarketTcpServer.h"
#include "netcommon.h"
#include <chrono>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>

namespace wire = hqmarket::market::v1;
namespace net
{
	struct CMarketTcpServer::CConnection
	{
			CMarketTcpServer* m_pOwner{nullptr};
			evutil_socket_t m_fd{-1};
			struct bufferevent* m_pEvent{nullptr};
			CFrameCodec m_codec;
			bool m_bAuthenticated{false};
	};

	static std::int64_t NowMilliseconds()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
			.count();
	}

	static market::Exchange FromWire(wire::Exchange value)
	{
		return static_cast<market::Exchange>(static_cast<int>(value));
	}
	static market::Channel FromWire(wire::Channel value)
	{
		return static_cast<market::Channel>(static_cast<int>(value));
	}
	static wire::Exchange ToWire(market::Exchange value)
	{
		return static_cast<wire::Exchange>(static_cast<int>(value));
	}

	CMarketTcpServer::CMarketTcpServer(int nPort, std::string strToken) : m_nPort(nPort), m_strToken(std::move(strToken))
	{
	}
	CMarketTcpServer::~CMarketTcpServer()
	{
		if (m_pListener != nullptr)
		{
			evconnlistener_free(m_pListener);
			m_pListener = nullptr;
		}
		std::lock_guard lock(m_mtxConnections);
		for (auto& [fd, connection] : m_connections)
		{
			if (connection->m_pEvent != nullptr)
			{
				bufferevent_free(connection->m_pEvent);
			}
		}
	}

	int CMarketTcpServer::Initialize()
	{
		struct sockaddr_in address;
		if (!FmtAddress(address, m_nPort))
		{
			return -1;
		}
		m_pListener = evconnlistener_new_bind(GetNet(), AcceptCallback, this, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
											  reinterpret_cast<sockaddr*>(&address), sizeof(address));
		return (m_pListener != nullptr) ? 0 : -1;
	}

	void CMarketTcpServer::SetSubscriptionHandler(_TySubscriptionHandler handler)
	{
		m_subscriptionHandler = std::move(handler);
	}
	void CMarketTcpServer::AcceptCallback(evconnlistener*, evutil_socket_t fd, sockaddr* pAddress, int nLength,
										  void* pContext)
	{
		static_cast<CMarketTcpServer*>(pContext)->OnAccept(fd, pAddress, nLength);
	}
	void CMarketTcpServer::ReadCallback(bufferevent*, void* pContext)
	{
		auto& connection = *static_cast<CConnection*>(pContext);
		connection.m_pOwner->OnRead(connection);
	}
	void CMarketTcpServer::EventCallback(bufferevent*, short nEvents, void* pContext)
	{
		auto& connection = *static_cast<CConnection*>(pContext);
		connection.m_pOwner->OnEvent(connection, nEvents);
	}

	void CMarketTcpServer::OnAccept(evutil_socket_t fd, sockaddr*, int)
	{
		std::unique_ptr<CConnection> connection = std::make_unique<CConnection>();
		connection->m_pOwner = this;
		connection->m_fd = fd;
		connection->m_pEvent = bufferevent_socket_new(GetNet(), fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
		if (connection->m_pEvent == nullptr)
		{
			evutil_closesocket(fd);
			return;
		}
		bufferevent_setcb(connection->m_pEvent, ReadCallback, nullptr, EventCallback, connection.get());
		bufferevent_enable(connection->m_pEvent, EV_READ | EV_WRITE);
		std::lock_guard lock(m_mtxConnections);
		m_connections.emplace(fd, std::move(connection));
	}

	void CMarketTcpServer::OnRead(CConnection& connection)
	{
		auto* input = bufferevent_get_input(connection.m_pEvent);
		std::size_t nLength = evbuffer_get_length(input);
		std::vector<unsigned char> data(nLength);
		evbuffer_remove(input, data.data(), data.size());
		std::vector<std::string> frames;
		if (!connection.m_codec.Append(data.data(), data.size(), frames))
		{
			Close(connection.m_fd);
			return;
		}
		for (const auto& frame : frames)
		{
			wire::MarketEnvelope envelope;
			if (!envelope.ParseFromString(frame))
			{
				Close(connection.m_fd);
				return;
			}
			Handle(connection, envelope);
		}
	}

	void CMarketTcpServer::Handle(CConnection& connection, const wire::MarketEnvelope& envelope)
	{
		wire::MarketEnvelope response;
		response.set_protocol_major(1);
		response.set_protocol_minor(0);
		response.set_request_id(envelope.request_id());
		if (envelope.protocol_major() != 1)
		{
			response.set_type(wire::ERROR);
			response.mutable_error()->set_code(1001);
			response.mutable_error()->set_message("unsupported protocol version");
			Send(connection, response);
			return;
		}
		if (envelope.type() == wire::AUTH_REQUEST)
		{
			connection.m_bAuthenticated = !m_strToken.empty() && envelope.auth_request().token() == m_strToken;
			response.set_type(wire::AUTH_RESPONSE);
			response.mutable_auth_response()->set_accepted(connection.m_bAuthenticated);
			response.mutable_auth_response()->set_reason(connection.m_bAuthenticated ? "ok" : "invalid token");
			Send(connection, response);
			return;
		}
		if (connection.m_bAuthenticated == false)
		{
			response.set_type(wire::ERROR);
			response.mutable_error()->set_code(1002);
			response.mutable_error()->set_message("authentication required");
			Send(connection, response);
			return;
		}
		if (envelope.type() == wire::HEARTBEAT)
		{
			response.set_type(wire::HEARTBEAT);
			response.mutable_heartbeat()->set_client_time_ms(envelope.heartbeat().client_time_ms());
			Send(connection, response);
			return;
		}
		bool bSubscribe = envelope.type() == wire::SUBSCRIBE_REQUEST;
		bool bUnsubscribe = envelope.type() == wire::UNSUBSCRIBE_REQUEST;
		if ((bSubscribe == false) && (bUnsubscribe == false))
		{
			return;
		}
		const auto& instruments =
			bSubscribe ? envelope.subscribe_request().instruments() : envelope.unsubscribe_request().instruments();
		const auto& channels =
			bSubscribe ? envelope.subscribe_request().channels() : envelope.unsubscribe_request().channels();
		std::vector<market::CSubscription> requested;
		for (const auto& instrument : instruments)
		{
			for (const auto channel : channels)
			{
				requested.push_back({{instrument.symbol(), FromWire(instrument.exchange())}, FromWire(channel)});
			}
		}
		auto changed = bSubscribe ? m_subscriptions.Subscribe(static_cast<std::uint64_t>(connection.m_fd), requested)
								  : m_subscriptions.Unsubscribe(static_cast<std::uint64_t>(connection.m_fd), requested);
		if (m_subscriptionHandler && !changed.empty())
		{
			m_subscriptionHandler(changed, bSubscribe);
		}
		response.set_type(wire::SUBSCRIPTION_ACK);
		for (const auto& subscription : requested)
		{
			auto* item = response.mutable_subscription_ack()->add_results();
			item->mutable_instrument()->set_symbol(subscription.m_instrument.m_strSymbol);
			item->mutable_instrument()->set_exchange(ToWire(subscription.m_instrument.m_exchange));
			item->set_channel(static_cast<wire::Channel>(static_cast<int>(subscription.m_channel)));
			item->set_accepted(true);
		}
		Send(connection, response);
	}

	void CMarketTcpServer::Send(CConnection& connection, wire::MarketEnvelope& envelope)
	{
		envelope.set_server_time_ms(NowMilliseconds());
		std::string payload;
		if (!envelope.SerializeToString(&payload))
		{
			return;
		}
		std::vector<std::uint8_t> frame = CFrameCodec::Encode(payload);
		bufferevent_write(connection.m_pEvent, frame.data(), frame.size());
	}

	void CMarketTcpServer::PublishQuote(const market::CQuote& quote, std::uint64_t nSequence)
	{
		wire::MarketEnvelope envelope;
		envelope.set_protocol_major(1);
		envelope.set_protocol_minor(0);
		envelope.set_type(wire::QUOTE);
		envelope.set_sequence(nSequence);
		auto* value = envelope.mutable_quote();
		value->mutable_instrument()->set_symbol(quote.m_instrument.m_strSymbol);
		value->mutable_instrument()->set_exchange(ToWire(quote.m_instrument.m_exchange));
		value->set_exchange_time_ms(quote.m_nExchangeTime);
		value->set_receive_time_ms(quote.m_nReceiveTime);
		value->set_last_price(quote.m_nLastPrice);
		value->set_open_price(quote.m_nOpenPrice);
		value->set_high_price(quote.m_nHighPrice);
		value->set_low_price(quote.m_nLowPrice);
		value->set_pre_close(quote.m_nPreClose);
		value->set_volume(quote.m_nVolume);
		value->set_turnover(quote.m_nTurnover);
		value->set_price_scale(quote.m_nPriceScale);
		value->set_source(quote.m_strSource);
		value->set_stale(quote.m_bStale);
		market::CSubscription subscription{quote.m_instrument, market::Channel::quote};
		std::lock_guard lock(m_mtxConnections);
		for (auto& [fd, connection] : m_connections)
		{
			if (connection->m_bAuthenticated && m_subscriptions.IsSubscribed(static_cast<std::uint64_t>(fd), subscription))
			{
				Send(*connection, envelope);
			}
		}
	}
	void CMarketTcpServer::PublishDepth(const market::CDepth& depth, std::uint64_t nSequence)
	{
		wire::MarketEnvelope envelope;
		envelope.set_protocol_major(1);
		envelope.set_protocol_minor(0);
		envelope.set_type(wire::DEPTH);
		envelope.set_sequence(nSequence);
		auto* value = envelope.mutable_depth();
		value->mutable_instrument()->set_symbol(depth.m_instrument.m_strSymbol);
		value->mutable_instrument()->set_exchange(ToWire(depth.m_instrument.m_exchange));
		value->set_exchange_time_ms(depth.m_nExchangeTime);
		value->set_receive_time_ms(depth.m_nReceiveTime);
		value->set_source(depth.m_strSource);
		value->set_stale(depth.m_bStale);
		for (const auto& level : depth.m_bids)
		{
			auto* item = value->add_bids();
			item->set_price(level.m_nPrice);
			item->set_volume(level.m_nVolume);
			item->set_price_scale(level.m_nPriceScale);
		}
		for (const auto& level : depth.m_asks)
		{
			auto* item = value->add_asks();
			item->set_price(level.m_nPrice);
			item->set_volume(level.m_nVolume);
			item->set_price_scale(level.m_nPriceScale);
		}
		market::CSubscription subscription{depth.m_instrument, market::Channel::depth};
		std::lock_guard lock(m_mtxConnections);
		for (auto& [fd, connection] : m_connections)
		{
			if (connection->m_bAuthenticated && m_subscriptions.IsSubscribed(static_cast<std::uint64_t>(fd), subscription))
			{
				Send(*connection, envelope);
			}
		}
	}

	void CMarketTcpServer::OnEvent(CConnection& connection, short nEvents)
	{
		if (nEvents & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
		{
			Close(connection.m_fd);
		}
	}
	void CMarketTcpServer::Close(evutil_socket_t fd)
	{
		{
			std::lock_guard lock(m_mtxConnections);
			std::unordered_map<evutil_socket_t, std::unique_ptr<CConnection>>::iterator iter = m_connections.find(fd);
			if (iter == m_connections.end())
			{
				return;
			}
			if (iter->second->m_pEvent != nullptr)
			{
				bufferevent_free(iter->second->m_pEvent);
				iter->second->m_pEvent = nullptr;
			}
			m_connections.erase(iter);
		}
		std::vector<market::CSubscription> removed = m_subscriptions.RemoveClient(static_cast<std::uint64_t>(fd));
		if (m_subscriptionHandler && !removed.empty())
		{
			m_subscriptionHandler(removed, false);
		}
	}
	std::size_t CMarketTcpServer::ClientCount() const
	{
		std::lock_guard lock(m_mtxConnections);
		return m_connections.size();
	}
} // namespace net
