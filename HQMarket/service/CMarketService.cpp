#include "CMarketService.h"
#include "../network/CNetPool.h"
#include "../network/CNetTools.h"
#include "../request/request.h"
#include "../common/defines.h"
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
		if ((m_pTcpServer == nullptr) || strToken.empty())
		{
			return false;
		}
		if (!m_storage.Open(root / "data" / "hqmarket.db"))
		{
			return false;
		}
		if (!m_python.Initialize(root / "runtime" / "python", root / "python"))
		{
			return false;
		}

		m_strToken = strToken;
		m_pTcpServer->RegisterHandler(
			[this](const std::unique_ptr<CRequest>& request)
			{
				return OnClientRequest(request);
			});
		net::CNetPool::InstancePtr()->RegisterDisconnectedHandler(
			[this](net::_TyConnectionId id)
			{
				OnClientDisconnected(id);
			});
		m_mootdx.SetQuoteHandler(
			[this](market::CQuote&& quote)
			{
				market::CInstrument instrument = quote.m_instrument;
				std::uint64_t sequence = m_cache.Update(std::move(quote));
				std::optional<market::CQuote> cached = m_cache.GetQuote(instrument);
				if (cached.has_value())
				{
					PublishQuote(*cached, sequence);
				}
			});
		m_mootdx.SetDepthHandler(
			[this](market::CDepth&& depth)
			{
				PublishDepth(depth, ++m_nDepthSequence);
			});
		if (!m_mootdx.Initialize())
		{
			return false;
		}
		m_akshare.Initialize();
		return true;
	}

	void CMarketService::Stop()
	{
		net::CNetPool::InstancePtr()->RegisterDisconnectedHandler({});
		m_mootdx.Stop();
		m_akshare.Stop();
		ThreadPoolPtr->ShutDown();
		m_python.Finalize();
		m_storage.Close();
	}

	int CMarketService::OnClientRequest(const std::unique_ptr<CRequest>& request)
	{
		if ((request == nullptr) || (request->GetConnectionId() < 0))
		{
			return 0;
		}
		net::_TyConnectionId id = request->GetConnectionId();
		wire::MarketEnvelope envelope;
		if (!envelope.ParseFromString(request->GetPayload()))
		{
			net::CNetPool::InstancePtr()->CloseAConnection(id);
			return 0;
		}
		HandleEnvelope(id, envelope);
		return 1;
	}

	void CMarketService::OnClientDisconnected(net::_TyConnectionId id)
	{
		net::utility::ReleaseConnectionBuffer(id);
		{
			std::lock_guard<std::mutex> lock(m_mtx_sessions);
			m_sessions.erase(id);
		}
		std::vector<market::CSubscription> removed = m_subscriptions.RemoveClient(id);
		if (!removed.empty())
		{
			m_mootdx.Unsubscribe(removed);
		}
	}

	void CMarketService::HandleEnvelope(net::_TyConnectionId id, const wire::MarketEnvelope& envelope)
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
			SendEnvelope(id, response);
			return;
		}
		if (envelope.type() == wire::AUTH_REQUEST)
		{
			bool authenticated = !m_strToken.empty() && (envelope.auth_request().token() == m_strToken);
			if (authenticated)
			{
				std::lock_guard<std::mutex> lock(m_mtx_sessions);
				m_sessions.try_emplace(id, CClientSession{true});
			}
			response.set_type(wire::AUTH_RESPONSE);
			response.mutable_auth_response()->set_accepted(authenticated);
			response.mutable_auth_response()->set_reason(authenticated ? "ok" : "invalid token");
			SendEnvelope(id, response);
			return;
		}
		if (!IsAuthenticated(id))
		{
			response.set_type(wire::ERROR);
			response.mutable_error()->set_code(1002);
			response.mutable_error()->set_message("authentication required");
			SendEnvelope(id, response);
			return;
		}
		if (envelope.type() == wire::HEARTBEAT)
		{
			response.set_type(wire::HEARTBEAT);
			response.mutable_heartbeat()->set_client_time_ms(envelope.heartbeat().client_time_ms());
			SendEnvelope(id, response);
			return;
		}

		bool subscribe = envelope.type() == wire::SUBSCRIBE_REQUEST;
		bool unsubscribe = envelope.type() == wire::UNSUBSCRIBE_REQUEST;
		if (!subscribe && !unsubscribe)
		{
			return;
		}
		const auto& instruments =
			subscribe ? envelope.subscribe_request().instruments() : envelope.unsubscribe_request().instruments();
		const auto& channels =
			subscribe ? envelope.subscribe_request().channels() : envelope.unsubscribe_request().channels();
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
			subscribe ? m_subscriptions.Subscribe(id, requested)
							  : m_subscriptions.Unsubscribe(id, requested);
		if (!changed.empty())
		{
			if (subscribe)
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
		SendEnvelope(id, response);
	}

	void CMarketService::SendEnvelope(net::_TyConnectionId id, wire::MarketEnvelope& envelope)
	{
		envelope.set_server_time_ms(NowMilliseconds());
		std::string payload;
		if (!envelope.SerializeToString(&payload))
		{
			return;
		}
		std::string frame = net::CFrameCodec::Encode(payload);
		net::CNetPool::InstancePtr()->SendData2Client(id, frame.data(), frame.size());
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
		for (net::_TyConnectionId id : AuthenticatedClients())
		{
			if (m_subscriptions.IsSubscribed(id, subscription))
			{
				SendEnvelope(id, envelope);
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
		for (net::_TyConnectionId id : AuthenticatedClients())
		{
			if (m_subscriptions.IsSubscribed(id, subscription))
			{
				SendEnvelope(id, envelope);
			}
		}
	}

	bool CMarketService::IsAuthenticated(net::_TyConnectionId id) const
	{
		std::lock_guard<std::mutex> lock(m_mtx_sessions);
		std::unordered_map<net::_TyConnectionId, CClientSession>::const_iterator session =
			m_sessions.find(id);
		return (session != m_sessions.end()) && session->second.m_bAuthenticated;
	}

	std::vector<net::_TyConnectionId> CMarketService::AuthenticatedClients() const
	{
		std::lock_guard<std::mutex> lock(m_mtx_sessions);
		std::vector<net::_TyConnectionId> clients;
		clients.reserve(m_sessions.size());
		for (const std::pair<const net::_TyConnectionId, CClientSession>& session : m_sessions)
		{
			if (session.second.m_bAuthenticated)
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
		out << "hqmarket_clients " << net::CNetPool::InstancePtr()->Count()
			<< "\nhqmarket_quotes_cached " << m_cache.QuoteCount() << "\n";
		return out.str();
	}

	std::string CMarketService::QuoteJson(const std::string& strInstrument) const
	{
		std::optional<market::CQuote> quote = m_cache.GetQuote(ParseInstrument(strInstrument));
		if (!quote.has_value())
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
			if (!first)
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
		if (values.empty())
		{
			values = m_akshare.QueryBars(instrument, channel, nBeginTime, nEndTime);
			if (!values.empty())
			{
				m_storage.UpsertBars(values);
			}
		}
		std::ostringstream out;
		out << '[';
		bool first = true;
		for (const market::CBar& bar : values)
		{
			if (!first)
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
