#include "CHQService.h"
#include "../python/CPythonRuntime.h"
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

		bool IsValidInstrument(const market::CInstrument& instrument)
		{
			return !instrument.m_strSymbol.empty() && (instrument.m_exchange != market::Exchange::unknown);
		}

		bool IsRealtimeChannel(market::Channel channel)
		{
			return (channel == market::Channel::quote) || (channel == market::Channel::depth);
		}

		void FillQuote(const market::CQuote& quote, wire::QuoteData* value)
		{
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
		}

		void FillBar(const market::CBar& bar, wire::BarData* value)
		{
			value->mutable_instrument()->set_symbol(bar.m_instrument.m_strSymbol);
			value->mutable_instrument()->set_exchange(ToWire(bar.m_instrument.m_exchange));
			value->set_channel(static_cast<wire::Channel>(static_cast<int>(bar.m_channel)));
			value->set_begin_time_ms(bar.m_nBeginTime);
			value->set_open_price(bar.m_nOpenPrice);
			value->set_high_price(bar.m_nHighPrice);
			value->set_low_price(bar.m_nLowPrice);
			value->set_close_price(bar.m_nClosePrice);
			value->set_volume(bar.m_nVolume);
			value->set_turnover(bar.m_nTurnover);
			value->set_price_scale(bar.m_nPriceScale);
			value->set_adjustment(bar.m_strAdjustment);
			value->set_source(bar.m_strSource);
		}

		template <typename T>
		bool SetPayload(CRequest& request, const T& payload)
		{
			std::string data;
			if (!payload.SerializeToString(&data))
			{
				return false;
			}
			request.SetPayload(data);
			return true;
		}

		void SetError(CRequest& response, int code, const std::string& message)
		{
			wire::ErrorData error;
			error.set_code(code);
			error.set_message(message);
			response.SetCmd("error");
			SetPayload(response, error);
		}
	} // namespace

	CMarketService::CMarketService(net::CTcpServer* pTcpServer, CPythonRuntime* pPythonRuntime) : m_pTcpServer(pTcpServer), m_pPythonRuntime(pPythonRuntime)
	{
	}

	bool CMarketService::Initialize(const std::string& strToken, const std::filesystem::path& root)
	{
		if ((nullptr == m_pTcpServer) || (nullptr == m_pPythonRuntime) || !m_pPythonRuntime->IsInitialized() || strToken.empty())
		{
			return false;
		}
		if (!m_recorder.Open(root / "data" / "hqmarket.db"))
		{
			return false;
		}
		m_strToken = strToken;
		m_pTcpServer->RegisterHandler([this](const net::CNetEvent& netEvent)
			{
				return OnNetEvent(netEvent);
			});
		m_mootdx.SetQuoteHandler([this](market::CQuote&& quote)
			{
				market::CInstrument instrument = quote.m_instrument;
				std::uint64_t sequence = m_cache.Update(std::move(quote));
				std::optional<market::CQuote> cached = m_cache.GetQuote(instrument);
				if (cached.has_value())
				{
					PublishQuote(*cached, sequence);
				}
			});
		m_mootdx.SetDepthHandler([this](market::CDepth&& depth)
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

	int CMarketService::OnNetEvent(const net::CNetEvent& netEvent)
	{
		if (netEvent.m_event == net::em_event::request)
		{
			return OnClientRequest(netEvent.m_request);
		}

		if (netEvent.m_event == net::em_event::disconnected)
		{
			OnClientDisconnected(netEvent.m_connection_id);
		}
		return 1;
	}

	void CMarketService::Stop()
	{
		m_mootdx.Stop();
		m_akshare.Stop();
		m_recorder.Close();
	}

	int CMarketService::OnClientRequest(const std::unique_ptr<CRequest>& request)
	{
		if ((request == nullptr) || (request->GetConnectionId() < 0))
		{
			return 0;
		}
		HandleRequest(request->GetConnectionId(), *request);
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

	void CMarketService::HandleRequest(net::_TyConnectionId id, const CRequest& request)
	{
		CRequest response;
		response.SetRequestId(request.GetRequestId());
		const std::string& command = request.GetCmd();
		const std::string& payload = request.GetPayload();
		if (command == "auth")
		{
			wire::AuthRequest auth;
			bool authenticated = auth.ParseFromString(payload) && !m_strToken.empty() && (auth.token() == m_strToken);
			if (authenticated)
			{
				std::lock_guard<std::mutex> lock(m_mtx_sessions);
				m_sessions.try_emplace(id, CClientSession{true});
			}
			wire::AuthResponse result;
			result.set_accepted(authenticated);
			result.set_reason(authenticated ? "ok" : "invalid token");
			response.SetCmd("auth_response");
			SetPayload(response, result);
			SendRequest(id, response);
			return;
		}
		if (!IsAuthenticated(id))
		{
			SetError(response, 1002, "authentication required");
			SendRequest(id, response);
			return;
		}
		if (command == "heartbeat")
		{
			wire::HeartbeatData heartbeat;
			if (!heartbeat.ParseFromString(payload))
			{
				SetError(response, 1005, "invalid heartbeat payload");
			}
			else
			{
				response.SetCmd("heartbeat");
				SetPayload(response, heartbeat);
			}
			SendRequest(id, response);
			return;
		}
		if (command == "query")
		{
			HandleQuery(id, request);
			return;
		}

		bool subscribe = command == "subscribe";
		bool unsubscribe = command == "unsubscribe";
		if (!subscribe && !unsubscribe)
		{
			SetError(response, 1006, "unknown command");
			SendRequest(id, response);
			return;
		}
		wire::SubscribeRequest subscribeRequest;
		wire::UnsubscribeRequest unsubscribeRequest;
		if ((subscribe && !subscribeRequest.ParseFromString(payload)) ||
			(unsubscribe && !unsubscribeRequest.ParseFromString(payload)))
		{
			SetError(response, 1005, "invalid subscription payload");
			SendRequest(id, response);
			return;
		}
		const auto& instruments = subscribe ? subscribeRequest.instruments() : unsubscribeRequest.instruments();
		const auto& channels = subscribe ? subscribeRequest.channels() : unsubscribeRequest.channels();
		std::vector<market::CSubscription> requested;
		std::vector<market::CSubscription> valid;
		requested.reserve(static_cast<std::size_t>(instruments.size()) * static_cast<std::size_t>(channels.size()));
		for (const wire::Instrument& instrument : instruments)
		{
			for (int channel : channels)
			{
				market::CSubscription value{
					{instrument.symbol(), FromWire(instrument.exchange())}, FromWire(static_cast<wire::Channel>(channel))};
				requested.emplace_back(value);
				if (IsValidInstrument(value.m_instrument) && IsRealtimeChannel(value.m_channel))
				{
					valid.emplace_back(std::move(value));
				}
			}
		}
		std::vector<market::CSubscription> changed =
			subscribe ? m_subscriptions.Subscribe(id, valid)
							  : m_subscriptions.Unsubscribe(id, valid);
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

		wire::SubscriptionAck ack;
		for (const market::CSubscription& subscription : requested)
		{
			wire::SubscriptionResult* pResult = ack.add_results();
			pResult->mutable_instrument()->set_symbol(subscription.m_instrument.m_strSymbol);
			pResult->mutable_instrument()->set_exchange(ToWire(subscription.m_instrument.m_exchange));
			pResult->set_channel(static_cast<wire::Channel>(static_cast<int>(subscription.m_channel)));
			bool accepted = IsValidInstrument(subscription.m_instrument) && IsRealtimeChannel(subscription.m_channel);
			pResult->set_accepted(accepted);
			if (!accepted)
			{
				pResult->set_reason("invalid instrument or unsupported subscription channel");
			}
		}
		response.SetCmd("subscription_ack");
		SetPayload(response, ack);
		SendRequest(id, response);
		if (subscribe)
		{
			for (const market::CSubscription& subscription : valid)
			{
				if (subscription.m_channel != market::Channel::quote)
				{
					continue;
				}
				std::optional<market::CQuote> quote = m_cache.GetQuote(subscription.m_instrument);
				if (quote.has_value())
				{
					wire::QuoteData snapshotPayload;
					FillQuote(*quote, &snapshotPayload);
					CRequest snapshot;
					snapshot.SetCmd("quote");
					SetPayload(snapshot, snapshotPayload);
					SendRequest(id, snapshot);
				}
			}
		}
	}

	void CMarketService::HandleQuery(net::_TyConnectionId id, const CRequest& requestData)
	{
		wire::QueryRequest request;
		CRequest response;
		response.SetRequestId(requestData.GetRequestId());
		if (!request.ParseFromString(requestData.GetPayload()))
		{
			SetError(response, 1005, "invalid query payload");
			SendRequest(id, response);
			return;
		}
		market::CInstrument instrument{request.instrument().symbol(), FromWire(request.instrument().exchange())};
		market::Channel channel = FromWire(request.channel());
		if (!IsValidInstrument(instrument) || ((channel != market::Channel::quote) &&
			(channel != market::Channel::bar_1m) && (channel != market::Channel::bar_1d)))
		{
			SetError(response, 1003, "invalid instrument or unsupported query channel");
			SendRequest(id, response);
			return;
		}
		wire::QueryResponse result;
		result.mutable_instrument()->CopyFrom(request.instrument());
		result.set_channel(request.channel());
		if (channel == market::Channel::quote)
		{
			std::optional<market::CQuote> quote = m_cache.GetQuote(instrument);
			result.set_found(quote.has_value());
			if (quote.has_value())
			{
				FillQuote(*quote, result.mutable_quote());
			}
		}
		else
		{
			std::int64_t begin = request.begin_time_ms();
			std::int64_t end = request.end_time_ms() > 0 ? request.end_time_ms() : NowMilliseconds();
			if ((begin < 0) || (end < begin))
			{
				SetError(response, 1004, "invalid query time range");
				SendRequest(id, response);
				return;
			}
			std::vector<market::CBar> bars = m_recorder.QueryBars(instrument, channel, begin, end);
			if (bars.empty())
			{
				bars = m_akshare.QueryBars(instrument, channel, begin, end);
				if (!bars.empty())
				{
					m_recorder.UpsertBars(bars);
				}
			}
			result.set_found(!bars.empty());
			for (const market::CBar& bar : bars)
			{
				FillBar(bar, result.add_bars());
			}
		}
		response.SetCmd("query_response");
		SetPayload(response, result);
		SendRequest(id, response);
	}

	void CMarketService::SendRequest(net::_TyConnectionId id, CRequest& request)
	{
		request.SetServerTime(NowMilliseconds());
		std::string payload;
		if (!request.Serialize(&payload))
		{
			return;
		}
		std::string frame = net::CFrameCodec::Encode(payload);
		net::CNetPool::InstancePtr()->SendData2Client(id, frame.data(), frame.size());
	}

	void CMarketService::PublishQuote(const market::CQuote& quote, std::uint64_t nSequence)
	{
		wire::QuoteData payload;
		FillQuote(quote, &payload);
		CRequest request;
		request.SetCmd("quote");
		request.SetSequence(nSequence);
		SetPayload(request, payload);
		market::CSubscription subscription{quote.m_instrument, market::Channel::quote};
		for (net::_TyConnectionId id : AuthenticatedClients())
		{
			if (m_subscriptions.IsSubscribed(id, subscription))
			{
				SendRequest(id, request);
			}
		}
	}

	void CMarketService::PublishDepth(const market::CDepth& depth, std::uint64_t nSequence)
	{
		wire::DepthData payload;
		wire::DepthData* pValue = &payload;
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
		CRequest request;
		request.SetCmd("depth");
		request.SetSequence(nSequence);
		SetPayload(request, payload);
		market::CSubscription subscription{depth.m_instrument, market::Channel::depth};
		for (net::_TyConnectionId id : AuthenticatedClients())
		{
			if (m_subscriptions.IsSubscribed(id, subscription))
			{
				SendRequest(id, request);
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
			<< "\",\"python\":" << (((nullptr != m_pPythonRuntime) && m_pPythonRuntime->IsInitialized()) ? "true" : "false")
			<< ",\"sqlite\":" << (m_recorder.IsOpen() ? "true" : "false")
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
		std::vector<market::CBar> values = m_recorder.QueryBars(instrument, channel, nBeginTime, nEndTime);
		if (values.empty())
		{
			values = m_akshare.QueryBars(instrument, channel, nBeginTime, nEndTime);
			if (!values.empty())
			{
				m_recorder.UpsertBars(values);
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
