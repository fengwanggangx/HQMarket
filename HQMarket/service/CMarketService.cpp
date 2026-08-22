#include "CMarketService.h"
#include <chrono>
#include <sstream>
#include <utility>

namespace wire = hqmarket::market::v1;

namespace service
{
	namespace
	{
		std::int64_t NowMilliseconds()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
					   std::chrono::system_clock::now().time_since_epoch())
				.count();
		}

		market::Exchange FromWire(wire::Exchange value)
		{
			return static_cast<market::Exchange>(static_cast<int>(value));
		}

		market::Channel FromWire(wire::Channel value)
		{
			return static_cast<market::Channel>(static_cast<int>(value));
		}

		wire::Exchange ToWire(market::Exchange value)
		{
			return static_cast<wire::Exchange>(static_cast<int>(value));
		}

		market::CInstrument ParseInstrument(const std::string& value)
		{
			market::CInstrument result;
			std::size_t dot = value.rfind('.');
			if (dot == std::string::npos)
			{
				return result;
			}
			result.m_strSymbol = value.substr(0, dot);
			std::string exchange = value.substr(dot + 1);
			if (exchange == "SSE")
			{
				result.m_exchange = market::Exchange::sse;
			}
			else if (exchange == "SZSE")
			{
				result.m_exchange = market::Exchange::szse;
			}
			else if (exchange == "BSE")
			{
				result.m_exchange = market::Exchange::bse;
			}
			else if (exchange == "HKEX")
			{
				result.m_exchange = market::Exchange::hkex;
			}
			return result;
		}
	} // namespace

	CMarketService::CMarketService(net::CTcpServer* pTcpServer) : m_pTcpServer(pTcpServer)
	{
	}

	bool CMarketService::Initialize(const std::string& strToken, const std::filesystem::path& root)
	{
		if ((m_pTcpServer == nullptr) || (strToken.empty() == true))
		{
			return false;
		}
		if (m_storage.Open(root / "data" / "hqmarket.db") == false)
		{
			return false;
		}
		if (m_python.Initialize(root / "runtime" / "python", root / "python") == false)
		{
			return false;
		}

		m_strToken = strToken;
		m_pTcpServer->RegisterConnectedHandler(
			[this](net::CTcpServer::ConnectionId connectionId)
			{
				OnClientConnected(connectionId);
			});
		m_pTcpServer->RegisterDataHandler(
			[this](net::CTcpServer::ConnectionId connectionId, std::vector<std::uint8_t>&& data)
			{
				OnClientData(connectionId, std::move(data));
			});
		m_pTcpServer->RegisterDisconnectedHandler(
			[this](net::CTcpServer::ConnectionId connectionId)
			{
				OnClientDisconnected(connectionId);
			});
		m_mootdx.SetQuoteHandler(
			[this](market::CQuote&& quote)
			{
				market::CInstrument instrument = quote.m_instrument;
				std::uint64_t sequence = m_cache.Update(std::move(quote));
				std::optional<market::CQuote> cached = m_cache.GetQuote(instrument);
				if (cached.has_value() == true)
				{
					PublishQuote(*cached, sequence);
				}
			});
		m_mootdx.SetDepthHandler(
			[this](market::CDepth&& depth)
			{
				PublishDepth(depth, ++m_nDepthSequence);
			});
		if (m_mootdx.Initialize() == false)
		{
			return false;
		}
		m_akshare.Initialize();
		return true;
	}

	void CMarketService::Stop()
	{
		m_mootdx.Stop();
		m_akshare.Stop();
		m_python.Finalize();
		m_storage.Close();
	}

	void CMarketService::OnClientConnected(net::CTcpServer::ConnectionId connectionId)
	{
		std::lock_guard<std::mutex> lock(m_mtxSessions);
		m_sessions.try_emplace(connectionId);
	}

	void CMarketService::OnClientData(net::CTcpServer::ConnectionId connectionId, std::vector<std::uint8_t>&& data)
	{
		std::vector<std::string> frames;
		bool valid = false;
		{
			std::lock_guard<std::mutex> lock(m_mtxSessions);
			std::unordered_map<net::CTcpServer::ConnectionId, CClientSession>::iterator session =
				m_sessions.find(connectionId);
			if (session == m_sessions.end())
			{
				return;
			}
			valid = session->second.m_codec.Append(data.data(), data.size(), frames);
		}
		if (valid == false)
		{
			m_pTcpServer->Close(connectionId);
			return;
		}
		for (const std::string& frame : frames)
		{
			wire::MarketEnvelope envelope;
			if (envelope.ParseFromString(frame) == false)
			{
				m_pTcpServer->Close(connectionId);
				return;
			}
			HandleEnvelope(connectionId, envelope);
		}
	}

	void CMarketService::OnClientDisconnected(net::CTcpServer::ConnectionId connectionId)
	{
		{
			std::lock_guard<std::mutex> lock(m_mtxSessions);
			m_sessions.erase(connectionId);
		}
		std::vector<market::CSubscription> removed = m_subscriptions.RemoveClient(connectionId);
		if (removed.empty() == false)
		{
			m_mootdx.Unsubscribe(removed);
		}
	}

	void CMarketService::HandleEnvelope(net::CTcpServer::ConnectionId connectionId, const wire::MarketEnvelope& envelope)
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
			SendEnvelope(connectionId, response);
			return;
		}
		if (envelope.type() == wire::AUTH_REQUEST)
		{
			bool authenticated = (m_strToken.empty() == false) && (envelope.auth_request().token() == m_strToken);
			{
				std::lock_guard<std::mutex> lock(m_mtxSessions);
				std::unordered_map<net::CTcpServer::ConnectionId, CClientSession>::iterator session =
					m_sessions.find(connectionId);
				if (session != m_sessions.end())
				{
					session->second.m_bAuthenticated = authenticated;
				}
			}
			response.set_type(wire::AUTH_RESPONSE);
			response.mutable_auth_response()->set_accepted(authenticated);
			response.mutable_auth_response()->set_reason(authenticated == true ? "ok" : "invalid token");
			SendEnvelope(connectionId, response);
			return;
		}
		if (IsAuthenticated(connectionId) == false)
		{
			response.set_type(wire::ERROR);
			response.mutable_error()->set_code(1002);
			response.mutable_error()->set_message("authentication required");
			SendEnvelope(connectionId, response);
			return;
		}
		if (envelope.type() == wire::HEARTBEAT)
		{
			response.set_type(wire::HEARTBEAT);
			response.mutable_heartbeat()->set_client_time_ms(envelope.heartbeat().client_time_ms());
			SendEnvelope(connectionId, response);
			return;
		}

		bool subscribe = envelope.type() == wire::SUBSCRIBE_REQUEST;
		bool unsubscribe = envelope.type() == wire::UNSUBSCRIBE_REQUEST;
		if ((subscribe == false) && (unsubscribe == false))
		{
			return;
		}
		const auto& instruments =
			subscribe == true ? envelope.subscribe_request().instruments() : envelope.unsubscribe_request().instruments();
		const auto& channels =
			subscribe == true ? envelope.subscribe_request().channels() : envelope.unsubscribe_request().channels();
		std::vector<market::CSubscription> requested;
		requested.reserve(static_cast<std::size_t>(instruments.size()) * static_cast<std::size_t>(channels.size()));
		for (const wire::Instrument& instrument : instruments)
		{
			for (int channel : channels)
			{
				requested.emplace_back(market::CSubscription{
					{instrument.symbol(), FromWire(instrument.exchange())}, FromWire(static_cast<wire::Channel>(channel))});
			}
		}
		std::vector<market::CSubscription> changed =
			subscribe == true ? m_subscriptions.Subscribe(connectionId, requested)
							  : m_subscriptions.Unsubscribe(connectionId, requested);
		if (changed.empty() == false)
		{
			if (subscribe == true)
			{
				m_mootdx.Subscribe(changed);
			}
			else
			{
				m_mootdx.Unsubscribe(changed);
			}
		}

		response.set_type(wire::SUBSCRIPTION_ACK);
		for (const market::CSubscription& subscription : requested)
		{
			wire::SubscriptionResult* pResult = response.mutable_subscription_ack()->add_results();
			pResult->mutable_instrument()->set_symbol(subscription.m_instrument.m_strSymbol);
			pResult->mutable_instrument()->set_exchange(ToWire(subscription.m_instrument.m_exchange));
			pResult->set_channel(static_cast<wire::Channel>(static_cast<int>(subscription.m_channel)));
			pResult->set_accepted(true);
		}
		SendEnvelope(connectionId, response);
	}

	void CMarketService::SendEnvelope(net::CTcpServer::ConnectionId connectionId, wire::MarketEnvelope& envelope)
	{
		envelope.set_server_time_ms(NowMilliseconds());
		std::string payload;
		if (envelope.SerializeToString(&payload) == false)
		{
			return;
		}
		std::string frame = net::CFrameCodec::Encode(payload);
		m_pTcpServer->Send(connectionId, frame.data(), frame.size());
	}

	void CMarketService::PublishQuote(const market::CQuote& quote, std::uint64_t nSequence)
	{
		wire::MarketEnvelope envelope;
		envelope.set_protocol_major(1);
		envelope.set_protocol_minor(0);
		envelope.set_type(wire::QUOTE);
		envelope.set_sequence(nSequence);
		wire::QuoteData* pValue = envelope.mutable_quote();
		pValue->mutable_instrument()->set_symbol(quote.m_instrument.m_strSymbol);
		pValue->mutable_instrument()->set_exchange(ToWire(quote.m_instrument.m_exchange));
		pValue->set_exchange_time_ms(quote.m_nExchangeTime);
		pValue->set_receive_time_ms(quote.m_nReceiveTime);
		pValue->set_last_price(quote.m_nLastPrice);
		pValue->set_open_price(quote.m_nOpenPrice);
		pValue->set_high_price(quote.m_nHighPrice);
		pValue->set_low_price(quote.m_nLowPrice);
		pValue->set_pre_close(quote.m_nPreClose);
		pValue->set_volume(quote.m_nVolume);
		pValue->set_turnover(quote.m_nTurnover);
		pValue->set_price_scale(quote.m_nPriceScale);
		pValue->set_source(quote.m_strSource);
		pValue->set_stale(quote.m_bStale);
		market::CSubscription subscription{quote.m_instrument, market::Channel::quote};
		for (net::CTcpServer::ConnectionId connectionId : AuthenticatedClients())
		{
			if (m_subscriptions.IsSubscribed(connectionId, subscription) == true)
			{
				SendEnvelope(connectionId, envelope);
			}
		}
	}

	void CMarketService::PublishDepth(const market::CDepth& depth, std::uint64_t nSequence)
	{
		wire::MarketEnvelope envelope;
		envelope.set_protocol_major(1);
		envelope.set_protocol_minor(0);
		envelope.set_type(wire::DEPTH);
		envelope.set_sequence(nSequence);
		wire::DepthData* pValue = envelope.mutable_depth();
		pValue->mutable_instrument()->set_symbol(depth.m_instrument.m_strSymbol);
		pValue->mutable_instrument()->set_exchange(ToWire(depth.m_instrument.m_exchange));
		pValue->set_exchange_time_ms(depth.m_nExchangeTime);
		pValue->set_receive_time_ms(depth.m_nReceiveTime);
		pValue->set_source(depth.m_strSource);
		pValue->set_stale(depth.m_bStale);
		for (const market::CPriceLevel& level : depth.m_bids)
		{
			wire::PriceLevel* pLevel = pValue->add_bids();
			pLevel->set_price(level.m_nPrice);
			pLevel->set_volume(level.m_nVolume);
			pLevel->set_price_scale(level.m_nPriceScale);
		}
		for (const market::CPriceLevel& level : depth.m_asks)
		{
			wire::PriceLevel* pLevel = pValue->add_asks();
			pLevel->set_price(level.m_nPrice);
			pLevel->set_volume(level.m_nVolume);
			pLevel->set_price_scale(level.m_nPriceScale);
		}
		market::CSubscription subscription{depth.m_instrument, market::Channel::depth};
		for (net::CTcpServer::ConnectionId connectionId : AuthenticatedClients())
		{
			if (m_subscriptions.IsSubscribed(connectionId, subscription) == true)
			{
				SendEnvelope(connectionId, envelope);
			}
		}
	}

	bool CMarketService::IsAuthenticated(net::CTcpServer::ConnectionId connectionId) const
	{
		std::lock_guard<std::mutex> lock(m_mtxSessions);
		std::unordered_map<net::CTcpServer::ConnectionId, CClientSession>::const_iterator session =
			m_sessions.find(connectionId);
		return (session != m_sessions.end()) && (session->second.m_bAuthenticated == true);
	}

	std::vector<net::CTcpServer::ConnectionId> CMarketService::AuthenticatedClients() const
	{
		std::lock_guard<std::mutex> lock(m_mtxSessions);
		std::vector<net::CTcpServer::ConnectionId> clients;
		clients.reserve(m_sessions.size());
		for (const std::pair<const net::CTcpServer::ConnectionId, CClientSession>& session : m_sessions)
		{
			if (session.second.m_bAuthenticated == true)
			{
				clients.emplace_back(session.first);
			}
		}
		return clients;
	}

	std::string CMarketService::HealthJson() const
	{
		market::CProviderStatus realtime = m_mootdx.GetStatus();
		market::CProviderStatus history = m_akshare.GetStatus();
		std::ostringstream out;
		out << "{\"status\":\"" << ((realtime.m_bHealthy && history.m_bHealthy) ? "ok" : "degraded")
			<< "\",\"python\":" << (m_python.IsInitialized() ? "true" : "false")
			<< ",\"sqlite\":" << (m_storage.IsOpen() ? "true" : "false")
			<< ",\"mootdx\":" << (realtime.m_bHealthy ? "true" : "false")
			<< ",\"akshare\":" << (history.m_bHealthy ? "true" : "false") << "}";
		return out.str();
	}

	std::string CMarketService::MetricsText() const
	{
		std::ostringstream out;
		out << "hqmarket_clients " << (m_pTcpServer != nullptr ? m_pTcpServer->ClientCount() : 0)
			<< "\nhqmarket_quotes_cached " << m_cache.QuoteCount() << "\n";
		return out.str();
	}

	std::string CMarketService::QuoteJson(const std::string& strInstrument) const
	{
		std::optional<market::CQuote> quote = m_cache.GetQuote(ParseInstrument(strInstrument));
		if (quote.has_value() == false)
		{
			return "{}";
		}
		std::ostringstream out;
		out << "{\"instrument\":\"" << quote->m_instrument.Key() << "\",\"exchange_time_ms\":" << quote->m_nExchangeTime
			<< ",\"receive_time_ms\":" << quote->m_nReceiveTime << ",\"last_price\":" << quote->m_nLastPrice
			<< ",\"price_scale\":" << quote->m_nPriceScale << ",\"volume\":" << quote->m_nVolume
			<< ",\"turnover\":" << quote->m_nTurnover << ",\"source\":\"" << quote->m_strSource
			<< "\",\"stale\":" << (quote->m_bStale ? "true" : "false") << "}";
		return out.str();
	}

	std::string CMarketService::InstrumentsJson() const
	{
		std::vector<market::CInstrument> values = m_akshare.QueryInstruments();
		std::ostringstream out;
		out << '[';
		bool first = true;
		for (const market::CInstrument& instrument : values)
		{
			if (first == false)
			{
				out << ',';
			}
			first = false;
			out << "{\"instrument\":\"" << instrument.Key() << "\"}";
		}
		out << ']';
		return out.str();
	}

	std::string CMarketService::BarsJson(const std::string& strInstrument, market::Channel channel,
									 std::int64_t nBeginTime, std::int64_t nEndTime)
	{
		market::CInstrument instrument = ParseInstrument(strInstrument);
		std::vector<market::CBar> values = m_storage.QueryBars(instrument, channel, nBeginTime, nEndTime);
		if (values.empty() == true)
		{
			values = m_akshare.QueryBars(instrument, channel, nBeginTime, nEndTime);
			if (values.empty() == false)
			{
				m_storage.UpsertBars(values);
			}
		}
		std::ostringstream out;
		out << '[';
		bool first = true;
		for (const market::CBar& bar : values)
		{
			if (first == false)
			{
				out << ',';
			}
			first = false;
			out << "{\"begin_time_ms\":" << bar.m_nBeginTime << ",\"open\":" << bar.m_nOpenPrice
				<< ",\"high\":" << bar.m_nHighPrice << ",\"low\":" << bar.m_nLowPrice << ",\"close\":" << bar.m_nClosePrice
				<< ",\"volume\":" << bar.m_nVolume << ",\"price_scale\":" << bar.m_nPriceScale << "}";
		}
		out << ']';
		return out.str();
	}
} // namespace service
