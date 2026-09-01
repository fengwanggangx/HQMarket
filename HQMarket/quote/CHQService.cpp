#include "CHQService.h"
#include "../python/CPythonRuntime.h"
#include "../network/CNetPool.h"
#include "../network/CNetTools.h"
#include "../request/request.h"
#include "../common/defines.h"
#include <chrono>
#include <exception>
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

		wire::Exchange ToWire(market::Exchange value)
		{
			return static_cast<wire::Exchange>(static_cast<int>(value));
		}

		market::CSecurity ParseInstrument(const std::string& value)
		{
			market::CSecurity result;
			std::size_t dot = value.rfind('.');
			if (std::string::npos == dot)
			{
				return result;
			}
			result.m_strCode = value.substr(0, dot);
			std::string exchange = value.substr(dot + 1);
			if ("SSE" == exchange)
			{
				result.m_market = market::Exchange::sse;
			}
			else if ("SZSE" == exchange)
			{
				result.m_market = market::Exchange::szse;
			}
			else if ("BSE" == exchange)
			{
				result.m_market = market::Exchange::bse;
			}
			else if ("HKEX" == exchange)
			{
				result.m_market = market::Exchange::hkex;
			}
			return result;
		}

		bool IsValidInstrument(const market::CSecurity& security)
		{
			return security.IsValid();
		}

		bool IsRealtimeChannel(market::Channel channel)
		{
			return (market::Channel::quote == channel) || (market::Channel::depth == channel);
		}

		market::Channel ParseChannel(const std::string& value)
		{
			if ("quote" == value)
			{
				return market::Channel::quote;
			}
			if ("depth" == value)
			{
				return market::Channel::depth;
			}
			if ("bar_1m" == value)
			{
				return market::Channel::bar_1m;
			}
			if ("bar_1d" == value)
			{
				return market::Channel::bar_1d;
			}
			return market::Channel::unknown;
		}

		bool ParseMilliseconds(const std::string& value, std::int64_t& result)
		{
			try
			{
				std::size_t parsed = 0;
				result = std::stoll(value, &parsed);
				return parsed == value.size();
			}
			catch (const std::exception&)
			{
				return false;
			}
		}

		void FillQuote(const market::CQuote& quote, wire::QuoteData* value)
		{
			value->mutable_instrument()->set_symbol(quote.m_security.m_strCode);
			value->mutable_instrument()->set_exchange(ToWire(quote.m_security.m_market));
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
			value->mutable_instrument()->set_symbol(bar.m_security.m_strCode);
			value->mutable_instrument()->set_exchange(ToWire(bar.m_security.m_market));
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
		bool SetData(CRequest& request, const T& value, std::uint64_t requestId = 0, std::uint64_t sequence = 0)
		{
			std::unique_ptr<T> data = std::make_unique<T>(value);
			std::unique_ptr<CData> requestData = std::make_unique<CData>(T::descriptor()->full_name(), data.get());
			data.release();
			request.SetType(CRequest::Type::HQMARKET);
			request.SetReturnData("request_id", std::to_string(requestId));
			request.SetReturnData("sequence", std::to_string(sequence));
			request.SetReturnData("server_time_ms", std::to_string(NowMilliseconds()));
			request.SetData(std::move(requestData));
			return true;
		}

		void SetError(CRequest& response, std::uint64_t requestId, int code, const std::string& message)
		{
			wire::ErrorData error;
			error.set_code(code);
			error.set_message(message);
			response.SetCmd("error");
			response.SetReturnData("error_code", std::to_string(code));
			response.SetReturnData("error_message", message);
			SetData(response, error, requestId);
		}
	} // namespace

	CMarketService::CMarketService(net::CTcpServer* pTcpServer, CPythonRuntime* pPythonRuntime) : m_pTcpServer(pTcpServer), m_pPythonRuntime(pPythonRuntime)
	{
		m_handler =
		{
			{ "auth", std::bind_front(&CMarketService::HandleAuth, this) },
			{ "heartbeat", std::bind_front(&CMarketService::HandleHeartbeat, this) },
			{ "query_quote", std::bind_front(&CMarketService::HandleQuery, this) },
			{ "query_bars", std::bind_front(&CMarketService::HandleQuery, this) },
			{ "subscribe", std::bind_front(&CMarketService::HandleSubscription, this) },
			{ "unsubscribe", std::bind_front(&CMarketService::HandleSubscription, this) }
		};
	}

	bool CMarketService::Initialize(const std::string& strToken, const std::filesystem::path& root)
	{
		if ((nullptr == m_pTcpServer) || (nullptr == m_pPythonRuntime) || !m_pPythonRuntime->IsInitialized() || strToken.empty())
		{
			return false;
		}
		m_strToken = strToken;
		m_pTcpServer->RegisterHandler(std::bind_front(&CMarketService::OnNetEvent, this));
		m_broker.SetQuoteHandler([this](const market::CQuote& quote, std::uint64_t sequence)
			{
				PublishQuote(quote, sequence);
			});
		m_broker.SetDepthHandler([this](market::CDepth&& depth)
			{
				PublishDepth(depth, ++m_nDepthSequence);
			});
		return m_broker.Initialize(root);
	}

	int CMarketService::OnNetEvent(const net::CNetEvent& ev)
	{
		if (net::em_event::request == ev.m_event)
		{
			return OnClientRequest(ev.m_request);
		}

		if (net::em_event::disconnected == ev.m_event)
		{
			OnClientDisconnected(ev.m_connection_id);
		}
		return 1;
	}

	void CMarketService::Stop()
	{
		m_broker.Stop();
	}

	int CMarketService::OnClientRequest(const std::unique_ptr<CRequest>& request)
	{
		if ((nullptr == request) || (request->GetConnectionId() < 0))
		{
			return 0;
		}
		HandleRequest(request->GetConnectionId(), *request);
		return 1;
	}

	void CMarketService::OnClientDisconnected(net::_TyConnectionId id)
	{
		net::CNetPool::InstancePtr()->CloseAConnection(id);
		{
			std::lock_guard<std::mutex> lck(m_mtx_sessions);
			m_sessions.erase(id);
		}
		std::vector<market::CSubscription> removed = m_subscriptions.RemoveClient(id);
		if (!removed.empty())
		{
			m_broker.Unsubscribe(removed);
		}
	}

	void CMarketService::HandleRequest(net::_TyConnectionId id, CRequest& request)
	{
		std::string strCmd = request.GetCmd();
		auto mIter = m_handler.find(strCmd);

		if ((m_handler.end() != mIter) && ("auth" == strCmd))
		{
			mIter->second(id, request);
			return;
		}
		if (!IsAuthenticated(id))
		{
			CRequest response;
			SetError(response, request.GetId(), 1002, "authentication required");
			SendRequest(id, response);
			return;
		}
		if (m_handler.end() == mIter)
		{
			CRequest response;
			SetError(response, request.GetId(), 1006, "unknown command");
			SendRequest(id, response);
			return;
		}
		mIter->second(id, request);
	}

	bool CMarketService::HandleAuth(net::_TyConnectionId id, CRequest& request)
	{
		bool bAuthenticated = !m_strToken.empty() && (m_strToken == request.GetExtraData("token"));
		if (bAuthenticated)
		{
			std::lock_guard<std::mutex> lck(m_mtx_sessions);
			m_sessions.try_emplace(id, CClientSession{ true });
		}
		CRequest response;
		response.SetType(CRequest::Type::HQMARKET);
		response.SetCmd("auth");
		response.SetReturnData("accepted", bAuthenticated ? "true" : "false");
		response.SetReturnData("reason", bAuthenticated ? "ok" : "invalid token");
		response.SetReturnData("request_id", std::to_string(request.GetId()));
		response.SetReturnData("server_time_ms", std::to_string(NowMilliseconds()));
		SendRequest(id, response);
		return true;
	}

	bool CMarketService::HandleHeartbeat(net::_TyConnectionId id, CRequest& request)
	{
		CRequest response;
		std::int64_t clientTime = 0;
		if (!ParseMilliseconds(request.GetExtraData("client_time_ms"), clientTime))
		{
			SetError(response, request.GetId(), 1005, "invalid client_time_ms");
		}
		else
		{
			response.SetType(CRequest::Type::HQMARKET);
			response.SetCmd("heartbeat");
			response.SetReturnData("client_time_ms", std::to_string(clientTime));
			response.SetReturnData("request_id", std::to_string(request.GetId()));
			response.SetReturnData("server_time_ms", std::to_string(NowMilliseconds()));
		}
		SendRequest(id, response);
		return true;
	}

	bool CMarketService::HandleSubscription(net::_TyConnectionId id, CRequest& request)
	{
		CRequest response;
		std::uint64_t requestId = request.GetId();
		bool bSubscribe = "subscribe" == request.GetCmd();
		market::CSecurity instrument = ParseInstrument(request.GetExtraData("instrument"));
		market::Channel channel = ParseChannel(request.GetExtraData("channel"));
		market::CSubscription subscription{ instrument, channel };
		bool bAccepted = IsValidInstrument(instrument) && IsRealtimeChannel(channel);
		if (!bAccepted)
		{
			SetError(response, requestId, 1003, "invalid instrument or unsupported subscription channel");
			SendRequest(id, response);
			return false;
		}
		std::vector<market::CSubscription> requested{ subscription };
		std::vector<market::CSubscription> changed = bSubscribe ? m_subscriptions.Subscribe(id, requested) : m_subscriptions.Unsubscribe(id, requested);
		if (!changed.empty())
		{
			if (bSubscribe)
			{
				m_broker.Subscribe(changed);
			}
			else
			{
				m_broker.Unsubscribe(changed);
			}
		}

		wire::SubscriptionAck ack;
		wire::SubscriptionResult* pResult = ack.add_results();
		pResult->mutable_instrument()->set_symbol(instrument.m_strCode);
		pResult->mutable_instrument()->set_exchange(ToWire(instrument.m_market));
		pResult->set_channel(static_cast<wire::Channel>(static_cast<int>(channel)));
		pResult->set_accepted(true);
		response.SetCmd("subscription_ack");
		response.SetReturnData("accepted", "true");
		SetData(response, ack, requestId);
		SendRequest(id, response);
		if (bSubscribe && (market::Channel::quote == channel))
		{
			std::optional<market::CQuote> quote = m_broker.QueryQuote(instrument);
			if (quote.has_value())
			{
				wire::QuoteData quoteData;
				FillQuote(*quote, &quoteData);
				CRequest snapshot;
				snapshot.SetCmd("quote");
				SetData(snapshot, quoteData);
				SendRequest(id, snapshot);
			}
		}
		return true;
	}

	bool CMarketService::HandleQuery(net::_TyConnectionId id, CRequest& requestData)
	{
		CRequest response;
		std::uint64_t requestId = requestData.GetId();
		market::CSecurity instrument = ParseInstrument(requestData.GetExtraData("instrument"));
		market::Channel channel = "query_quote" == requestData.GetCmd()
			? market::Channel::quote : ParseChannel(requestData.GetExtraData("channel"));
		if (!IsValidInstrument(instrument) || ((market::Channel::quote != channel) &&
			(market::Channel::bar_1m != channel) && (market::Channel::bar_1d != channel)))
		{
			SetError(response, requestId, 1003, "invalid instrument or unsupported query channel");
			SendRequest(id, response);
			return false;
		}
		wire::QueryResponse result;
		wire::QueryResponse* pResult = &result;
		pResult->mutable_instrument()->set_symbol(instrument.m_strCode);
		pResult->mutable_instrument()->set_exchange(ToWire(instrument.m_market));
		pResult->set_channel(static_cast<wire::Channel>(static_cast<int>(channel)));
		if (market::Channel::quote == channel)
		{
			std::optional<market::CQuote> quote = m_broker.QueryQuote(instrument);
			pResult->set_found(quote.has_value());
			if (quote.has_value())
			{
				FillQuote(*quote, pResult->mutable_quote());
			}
		}
		else
		{
			std::int64_t begin = 0;
			std::int64_t end = 0;
			if (!ParseMilliseconds(requestData.GetExtraData("begin_time_ms"), begin) ||
				!ParseMilliseconds(requestData.GetExtraData("end_time_ms"), end))
			{
				SetError(response, requestId, 1004, "invalid query time range");
				SendRequest(id, response);
				return false;
			}
			end = 0 < end ? end : NowMilliseconds();
			if ((0 > begin) || (end < begin))
			{
				SetError(response, requestId, 1004, "invalid query time range");
				SendRequest(id, response);
				return false;
			}
			std::vector<market::CBar> bars = m_broker.QueryBars(instrument, channel, begin, end);
			pResult->set_found(!bars.empty());
			for (const market::CBar& bar : bars)
			{
				FillBar(bar, pResult->add_bars());
			}
		}
		response.SetCmd("query_response");
		SetData(response, result, requestId);
		SendRequest(id, response);
		return true;
	}

	void CMarketService::SendRequest(net::_TyConnectionId id, CRequest& request)
	{
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
		wire::QuoteData quoteData;
		FillQuote(quote, &quoteData);
		CRequest request;
		request.SetCmd("quote");
		SetData(request, quoteData, 0, nSequence);
		market::CSubscription subscription{ quote.m_security, market::Channel::quote };
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
		wire::DepthData depthData;
		wire::DepthData* pValue = &depthData;
		pValue->mutable_instrument()->set_symbol(depth.m_security.m_strCode);
		pValue->mutable_instrument()->set_exchange(ToWire(depth.m_security.m_market));
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
		SetData(request, depthData, 0, nSequence);
		market::CSubscription subscription{ depth.m_security, market::Channel::depth };
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
		std::lock_guard<std::mutex> lck(m_mtx_sessions);
		std::unordered_map<net::_TyConnectionId, CClientSession>::const_iterator session =
			m_sessions.find(id);
		return (m_sessions.end() != session) && session->second.m_bAuthenticated;
	}

	std::vector<net::_TyConnectionId> CMarketService::AuthenticatedClients() const
	{
		std::lock_guard<std::mutex> lck(m_mtx_sessions);
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
		market::CProviderStatus realtime = m_broker.RealtimeStatus();
		market::CProviderStatus history = m_broker.HistoryStatus();
		std::ostringstream out;
		out << "{\"status\":\"" << ((realtime.m_bHealthy && history.m_bHealthy) ? "ok" : "degraded")
			<< "\",\"python\":" << (((nullptr != m_pPythonRuntime) && m_pPythonRuntime->IsInitialized()) ? "true" : "false")
			<< ",\"sqlite\":" << (m_broker.IsRecorderOpen() ? "true" : "false")
			<< ",\"mootdx\":" << (realtime.m_bHealthy ? "true" : "false")
			<< ",\"akshare\":" << (history.m_bHealthy ? "true" : "false") << "}";
		return out.str();
	}

	std::string CMarketService::MetricsText() const
	{
		std::ostringstream out;
		out << "hqmarket_clients " << net::CNetPool::InstancePtr()->Count()
			<< "\nhqmarket_quotes_cached " << m_broker.QuoteCount() << "\n";
		return out.str();
	}

	std::string CMarketService::QuoteJson(const std::string& strInstrument) const
	{
		std::optional<market::CQuote> quote = m_broker.QueryQuote(ParseInstrument(strInstrument));
		if (!quote.has_value())
		{
			return "{}";
		}
		std::ostringstream out;
		out << "{\"instrument\":\"" << quote->m_security.String() << "\",\"exchange_time_ms\":" << quote->m_nExchangeTime
			<< ",\"receive_time_ms\":" << quote->m_nReceiveTime << ",\"last_price\":" << quote->m_nLastPrice
			<< ",\"price_scale\":" << quote->m_nPriceScale << ",\"volume\":" << quote->m_nVolume
			<< ",\"turnover\":" << quote->m_nTurnover << ",\"source\":\"" << quote->m_strSource
			<< "\",\"stale\":" << (quote->m_bStale ? "true" : "false") << "}";
		return out.str();
	}

	std::string CMarketService::InstrumentsJson() const
	{
		std::vector<market::CSecurity> values = m_broker.QueryInstruments();
		std::ostringstream out;
		out << '[';
		bool bFirst = true;
		for (const market::CSecurity& instrument : values)
		{
			if (!bFirst)
			{
				out << ',';
			}
			bFirst = false;
			out << "{\"instrument\":\"" << instrument.String() << "\"}";
		}
		out << ']';
		return out.str();
	}

	std::string CMarketService::BarsJson(const std::string& strInstrument, market::Channel channel,
									 std::int64_t nBeginTime, std::int64_t nEndTime)
	{
		market::CSecurity instrument = ParseInstrument(strInstrument);
		std::vector<market::CBar> values = m_broker.QueryBars(instrument, channel, nBeginTime, nEndTime);
		std::ostringstream out;
		out << '[';
		bool bFirst = true;
		for (const market::CBar& bar : values)
		{
			if (!bFirst)
			{
				out << ',';
			}
			bFirst = false;
			out << "{\"begin_time_ms\":" << bar.m_nBeginTime << ",\"open\":" << bar.m_nOpenPrice
				<< ",\"high\":" << bar.m_nHighPrice << ",\"low\":" << bar.m_nLowPrice << ",\"close\":" << bar.m_nClosePrice
				<< ",\"volume\":" << bar.m_nVolume << ",\"price_scale\":" << bar.m_nPriceScale << "}";
		}
		out << ']';
		return out.str();
	}
} // namespace service
