#include "CHQService.h"
#include "../common/utility.h"
#include "../database/CDBEngine.h"
#include "../database/IDataBase.h"
#include "../python/CPythonRuntime.h"
#include "../network/CNetPool.h"
#include "../network/CNetTools.h"
#include "../request/request.h"
#include "../common/defines.h"
#include <chrono>
#include <cctype>
#include <exception>
#include <sstream>
#include <utility>

namespace wire = hqmarket::market::v1;

namespace
{
		constexpr int InvalidRequest = 1001;
		constexpr int InvalidCredentials = 1002;
		constexpr int StorageUnavailable = 1004;
		constexpr std::size_t MinAccountLength = 3;
		constexpr std::size_t MaxAccountLength = 64;
		constexpr std::size_t MinPasswordLength = 8;
		constexpr std::size_t MaxPasswordLength = 128;
		constexpr std::chrono::hours TokenLifetime{ 24 };

		bool IsAccountValid(const std::string& strAccount)
		{
			if ((MinAccountLength > strAccount.size()) || (MaxAccountLength < strAccount.size()))
			{
				return false;
			}
			for (unsigned char character : strAccount)
			{
				if ((0 == std::isalnum(character)) && ('_' != character) && ('-' != character) && ('.' != character) && ('@' != character))
				{
					return false;
				}
			}
			return true;
		}

		bool IsPasswordValid(const std::string& strPassword)
		{
			return (MinPasswordLength <= strPassword.size()) && (MaxPasswordLength >= strPassword.size());
		}

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

		bool IsRealtimeChannel(market::Channel channel)
		{
			return (market::Channel::quote == channel) || (market::Channel::depth == channel);
		}

		market::Channel ParseChannel(const std::string& v)
		{
			if ("quote" == v)
			{
				return market::Channel::quote;
			}
			if ("depth" == v)
			{
				return market::Channel::depth;
			}
			if ("bar_1m" == v)
			{
				return market::Channel::bar_1m;
			}
			if ("bar_1d" == v)
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
			request.SetId(requestId);
			request.SetReturnData("request_id", std::to_string(requestId));
			request.SetReturnData("sequence", std::to_string(sequence));
			request.SetReturnData("server_time_ms", std::to_string(NowMilliseconds()));
			request.SetData(std::move(requestData));
			return true;
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

	bool CMarketService::Initialize(const std::filesystem::path& root)
	{
		if ((nullptr == m_pTcpServer) || (nullptr == m_pPythonRuntime) || !m_pPythonRuntime->IsInitialized())
		{
			return false;
		}
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
			OnClientRequest(ev.m_request->GetConnectionId(), *ev.m_request);
		}
		else if (net::em_event::disconnected == ev.m_event)
		{
			OnClientDisconnected(ev.m_connection_id);
		}
		return 1;
	}

	void CMarketService::Stop()
	{
		m_broker.Stop();
	}

	void CMarketService::OnClientDisconnected(net::_TyConnectionId id)
	{
		net::CNetPool::InstancePtr()->CloseAConnection(id);
		{
			std::lock_guard<std::mutex> lck(m_mtx_sessions);
			m_auth_clients.erase(id);
		}
		std::vector<market::CChannelInfo> removed = m_subscriptions.RemoveClient(id);
		if (!removed.empty())
		{
			m_broker.Unsubscribe(removed);
		}
	}

	void CMarketService::OnClientRequest(net::_TyConnectionId id, const CRequest& request)
	{
		std::string strCmd = request.GetCmd();
		const auto mIter = m_handler.find(strCmd);
		if (m_handler.end() == mIter)
		{
			net::SendError(id, request, 1006, "unknown command");
			return;
		}
		
		if ("auth" == strCmd)
		{
			mIter->second(id, request);
			return;
		}

		if (!IsAuthenticated(id))
		{
			net::SendError(id, request, 1002, "authentication required");
			return;
		}

		mIter->second(id, request);
	}

	bool CMarketService::HandleAuth(net::_TyConnectionId id, const CRequest& request)
	{
		std::string strToken = request.GetExtraData("token");
		if (!strToken.empty())
		{
			return HandleReAuth(id, request);
		}

		if ("auth" == request.GetCmd())
		{
			if (Login(id, request, strToken))
			{
				std::lock_guard<std::mutex> lck(m_mtx_sessions);
				m_auth_clients.emplace(id);
				m_client_tokens.insert_or_assign(strToken, std::chrono::steady_clock::now() + TokenLifetime);
			}
			return true;
		}
		net::SendError(id, request, InvalidRequest, "unsupported authentication request");
		return false;
	}

	bool CMarketService::HandleReAuth(net::_TyConnectionId id, const CRequest& request)
	{
		std::string strToken = request.GetExtraData("token");
		bool bAccepted = false;
		{
			std::lock_guard<std::mutex> lck(m_mtx_sessions);
			auto mIter = m_client_tokens.find(strToken);
			bAccepted = (m_client_tokens.end() != mIter) && (std::chrono::steady_clock::now() < mIter->second);
			if ((m_client_tokens.end() != mIter) && !bAccepted)
			{
				m_client_tokens.erase(mIter);
			}
			if (bAccepted)
			{
				m_auth_clients.emplace(id);
			}
		}
		if (bAccepted)
		{
			SendAuthResponse(id, request, 0, "认证成功");
			return true;
		}
		SendAuthResponse(id, request, InvalidCredentials, "登录状态已失效");
		return false;
	}

	bool CMarketService::Login(net::_TyConnectionId id, const CRequest& request, std::string& strToken)
	{
		std::string strAccount = request.GetExtraData("user");
		std::string strPassword = request.GetExtraData("password");
		if (!IsAccountValid(strAccount) || !IsPasswordValid(strPassword))
		{
			SendAuthResponse(id, request, InvalidCredentials, "账号或密码错误");
			return false;
		}

		db::_TyDBPtr db = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
		if (nullptr == db)
		{
			SendAuthResponse(id, request, StorageUnavailable, "用户数据库暂不可用");
			return false;
		}

		std::string strSql = "SELECT user_id, account FROM table_user WHERE account=" + utility::Utf8Literal(strAccount) + " AND password_hash=UNHEX(SHA2(CONCAT(password_salt,UNHEX('" + utility::ToHex(strPassword) + "')),256)) AND status=1 LIMIT 1";
		const db::_TyTableInfo& table = db->ExecQuery(strSql);
		if (table.second.empty())
		{
			SendAuthResponse(id, request, InvalidCredentials, "账号或密码错误");
			return false;
		}

		CRequest response;
		response.SetId(request.GetId());
		response.SetType(request.GetType());
		response.SetCmd(request.GetCmd());
		response.SetReturnData("status", "ok");
		response.SetReturnData("user_id", table.second.front().at(0));
		response.SetReturnData("account", table.second.front().at(1));
		strToken = utility::MakeSaltHex();
		response.SetReturnData("token", strToken);
		net::SendRequest(id, response);
		return true;
	}

	void CMarketService::SendAuthResponse(net::_TyConnectionId id, const CRequest& request, int nErrorCode, const std::string& strMessage) const
	{
		CRequest response;
		response.SetId(request.GetId());
		response.SetType(request.GetType());
		response.SetCmd(request.GetCmd());
		if (0 != nErrorCode)
		{
			net::SetError(response, nErrorCode, strMessage);
		}
		else
		{
			response.SetReturnData("status", "ok");
			response.SetReturnData("message", strMessage);
		}
		net::SendRequest(id, response);
	}

	bool CMarketService::HandleHeartbeat(net::_TyConnectionId id, const CRequest& request)
	{
		std::int64_t clientTime = 0;
		if (!ParseMilliseconds(request.GetExtraData("client_time_ms"), clientTime))
		{
			net::SendError(id, request, 1005, "invalid client_time_ms");
			return false;
		}
		CRequest response;
		response.SetType(CRequest::Type::HQMARKET);
		response.SetId(request.GetId());
		response.SetCmd("heartbeat");
		response.SetReturnData("client_time_ms", std::to_string(clientTime));
		response.SetReturnData("request_id", std::to_string(request.GetId()));
		response.SetReturnData("server_time_ms", std::to_string(NowMilliseconds()));
		net::SendRequest(id, response);
		return true;
	}

	bool CMarketService::HandleSubscription(net::_TyConnectionId id, const CRequest& request)
	{
		std::string strCmd = request.GetCmd();
		if (("subscribe" != strCmd) && ("unsubscribe" != strCmd))
		{
			net::SendError(id, request, 1001, "invalid cmd");
			return false;
		}

		bool bSubscribe = "subscribe" == strCmd;

		std::uint64_t requestId = request.GetId();

		market::CSecurity security = ParseInstrument(request.GetExtraData("security"));
		market::Channel channel = ParseChannel(request.GetExtraData("channel"));

		bool bAccepted = security.IsValid() && IsRealtimeChannel(channel);
		if (!bAccepted)
		{
			net::SendError(id, request, 1003, "invalid security or unsupported subscription channel");
			return false;
		}

		market::CChannelInfo subscription{security, channel};
		std::vector<market::CChannelInfo> requested{ subscription };
		std::vector<market::CChannelInfo> changed = bSubscribe ? m_subscriptions.Subscribe(id, requested) : m_subscriptions.Unsubscribe(id, requested);
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
		pResult->mutable_instrument()->set_symbol(security.m_strCode);
		pResult->mutable_instrument()->set_exchange(ToWire(security.m_market));
		pResult->set_channel(static_cast<wire::Channel>(static_cast<int>(channel)));
		pResult->set_accepted(true);

		CRequest response;
		response.SetCmd("subscription_ack");
		response.SetReturnData("accepted", "1");
		SetData(response, ack, requestId);
		net::SendRequest(id, response);
		if (bSubscribe && (market::Channel::quote == channel))
		{
			std::optional<market::CQuote> quote = m_broker.QueryQuote(security);
			if (quote.has_value())
			{
				wire::QuoteData quoteData;
				FillQuote(*quote, &quoteData);
				CRequest snapshot;
				snapshot.SetCmd("quote");
				SetData(snapshot, quoteData);
				net::SendRequest(id, snapshot);
			}
		}
		return true;
	}

	bool CMarketService::HandleQuery(net::_TyConnectionId id, const CRequest& requestData)
	{
		std::uint64_t requestId = requestData.GetId();
		std::string strCmd = requestData.GetCmd();

		market::CSecurity security = ParseInstrument(requestData.GetExtraData("security"));
		market::Channel channel = "query_quote" == strCmd ? market::Channel::quote : ParseChannel(requestData.GetExtraData("channel"));
		if (!security.IsValid() || ((market::Channel::quote != channel) && (market::Channel::bar_1m != channel) && (market::Channel::bar_1d != channel)))
		{
			net::SendError(id, requestData, 1003, "invalid security or unsupported query channel");
			return false;
		}
		wire::QueryResponse result;
		wire::QueryResponse* pResult = &result;
		pResult->mutable_instrument()->set_symbol(security.m_strCode);
		pResult->mutable_instrument()->set_exchange(ToWire(security.m_market));
		pResult->set_channel(static_cast<wire::Channel>(static_cast<int>(channel)));

		CRequest response;
		if (market::Channel::quote == channel)
		{
			std::optional<market::CQuote> quote = m_broker.QueryQuote(security);
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
			if (!ParseMilliseconds(requestData.GetExtraData("begin_time_ms"), begin) || !ParseMilliseconds(requestData.GetExtraData("end_time_ms"), end))
			{
				net::SetError(response, requestData, 1004, "invalid query time range");
				net::SendRequest(id, response);
				return false;
			}
			end = 0 < end ? end : NowMilliseconds();
			if ((0 > begin) || (end < begin))
			{
				net::SetError(response, requestData, 1004, "invalid query time range");
				net::SendRequest(id, response);
				return false;
			}
			std::vector<market::CBar> bars = m_broker.QueryBars(security, channel, begin, end);
			pResult->set_found(!bars.empty());
			for (const market::CBar& bar : bars)
			{
				FillBar(bar, pResult->add_bars());
			}
		}
		response.SetCmd("query_response");
		SetData(response, result, requestId);
		net::SendRequest(id, response);
		return true;
	}

	void CMarketService::PublishQuote(const market::CQuote& quote, std::uint64_t nSequence)
	{
		wire::QuoteData quoteData;
		FillQuote(quote, &quoteData);
		CRequest request;
		request.SetCmd("quote");
		SetData(request, quoteData, 0, nSequence);
		market::CChannelInfo subscription{ quote.m_security, market::Channel::quote };
		std::vector<net::_TyConnectionId> disconnected;
		for (net::_TyConnectionId id : AuthenticatedClients())
		{
			if (m_subscriptions.IsSubscribed(id, subscription))
			{
				if (!net::SendRequest(id, request))
				{
					disconnected.emplace_back(id);
				}
			}
		}
		for (net::_TyConnectionId id : disconnected)
		{
			OnClientDisconnected(id);
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
		market::CChannelInfo subscription{ depth.m_security, market::Channel::depth };
		std::vector<net::_TyConnectionId> disconnected;
		for (net::_TyConnectionId id : AuthenticatedClients())
		{
			if (m_subscriptions.IsSubscribed(id, subscription))
			{
				if (!net::SendRequest(id, request))
				{
					disconnected.emplace_back(id);
				}
			}
		}
		for (net::_TyConnectionId id : disconnected)
		{
			OnClientDisconnected(id);
		}
	}

	bool CMarketService::IsAuthenticated(net::_TyConnectionId id) const
	{
		std::lock_guard<std::mutex> lck(m_mtx_sessions);
		const auto mIter = m_auth_clients.find(id);
		return (m_auth_clients.end() != mIter);
	}

	std::vector<net::_TyConnectionId> CMarketService::AuthenticatedClients() const
	{
		std::vector<net::_TyConnectionId> clients;
		clients.reserve(m_auth_clients.size());

		{
			std::lock_guard<std::mutex> lck(m_mtx_sessions);
			for (const auto& v : m_auth_clients)
			{
				clients.emplace_back(v);
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
		out << "{\"security\":\"" << quote->m_security.String() << "\",\"exchange_time_ms\":" << quote->m_nExchangeTime
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
		for (const market::CSecurity& security : values)
		{
			if (!bFirst)
			{
				out << ',';
			}
			bFirst = false;
			out << "{\"security\":\"" << security.String() << "\"}";
		}
		out << ']';
		return out.str();
	}

	std::string CMarketService::BarsJson(const std::string& strInstrument, market::Channel channel,
									 std::int64_t nBeginTime, std::int64_t nEndTime)
	{
		market::CSecurity security = ParseInstrument(strInstrument);
		std::vector<market::CBar> values = m_broker.QueryBars(security, channel, nBeginTime, nEndTime);
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
