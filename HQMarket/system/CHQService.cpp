#include "CHQService.h"
#include "../common/utility.h"
#include "../database/CDBEngine.h"
#include "../database/IDataBase.h"
#include "../python/CPythonRuntime.h"
#include "../network/CNetPool.h"
#include "../network/CNetTools.h"
#include "../request/request.h"
#include "../request/request.pb.h"
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
		value->mutable_security()->set_symbol(quote.m_security.m_strCode);
		value->mutable_security()->set_exchange(ToWire(quote.m_security.m_market));
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
		value->mutable_security()->set_symbol(bar.m_security.m_strCode);
		value->mutable_security()->set_exchange(ToWire(bar.m_security.m_market));
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

	void FillSector(const market::CSector& sector, wire::SectorInfo* value)
	{
		value->set_code(sector.m_strCode);
		value->set_name(sector.m_strName);
		value->set_change_percent(sector.m_nChangePercent);
		value->set_percent_scale(sector.m_nPercentScale);
		value->set_rising_count(sector.m_nRisingCount);
		value->set_falling_count(sector.m_nFallingCount);
		value->set_flat_count(sector.m_nFlatCount);
		value->set_member_count(sector.m_nMemberCount);
		value->mutable_leading_security()->set_symbol(sector.m_leadingSecurity.m_strCode);
		value->mutable_leading_security()->set_exchange(ToWire(sector.m_leadingSecurity.m_market));
		value->set_leading_name(sector.m_strLeadingName);
		value->set_snapshot_time_ms(sector.m_nSnapshotTime);
	}

	void FillSecurityInfo(const market::CInstrument& instrument, wire::SecurityInfo* value)
	{
		value->mutable_security()->set_symbol(instrument.m_security.m_strCode);
		value->mutable_security()->set_exchange(ToWire(instrument.m_security.m_market));
		value->set_name(instrument.m_strName);
		value->set_status(instrument.m_strStatus);
		for (const std::string& strAlias : instrument.m_pinyinFullAliases)
		{
			value->add_pinyin_full_aliases(strAlias);
		}
		for (const std::string& strAlias : instrument.m_pinyinShortAliases)
		{
			value->add_pinyin_short_aliases(strAlias);
		}
	}

	void HashString(std::uint64_t& value, const std::string& strText)
	{
		for (unsigned char character : strText)
		{
			value = (value ^ character) * 1099511628211ULL;
		}
		value = (value ^ 0xffU) * 1099511628211ULL;
	}

	template <typename T>
	bool SetData(CRequest& request, const T& value, std::uint64_t requestId = 0, std::uint64_t sequence = 0)
	{
		request.SetType(CRequest::Type::HQMARKET);
		request.SetId(requestId);
		request.SetReturnData("request_id", std::to_string(requestId));
		request.SetReturnData("sequence", std::to_string(sequence));
		request.SetReturnData("server_time_ms", std::to_string(NowMilliseconds()));
		request.SetData(value);
		return true;
	}

} // namespace

CMarketService::CMarketService(net::CTcpServer* pTcpServer, CPythonRuntime* pPythonRuntime) : m_pTcpServer(pTcpServer), m_pPythonRuntime(pPythonRuntime)
{
	m_handler = {
		{ "auth", std::bind_front(&CMarketService::HandleAuth, this) },
		{ "heartbeat", std::bind_front(&CMarketService::HandleHeartbeat, this) },
		{ "query_quote", std::bind_front(&CMarketService::HandleQuery, this) },
		{ "query_bars", std::bind_front(&CMarketService::HandleQuery, this) },
		{ "query_securities", std::bind_front(&CMarketService::HandleQuery, this) },
		{ "query_sectors", std::bind_front(&CMarketService::HandleQuery, this) },
		{ "query_sector_constituents", std::bind_front(&CMarketService::HandleQuery, this) },
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
							 { PublishQuote(quote, sequence); });
	m_broker.SetDepthHandler([this](market::CDepth&& depth)
							 { PublishDepth(depth, ++m_nDepthSequence); });
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

void CMarketService::OnClientRequest(net::_TyConnectionId id, const CRequest& req)
{
	std::string strCmd = req.GetCmd();
	const auto mIter = m_handler.find(strCmd);
	if (m_handler.end() == mIter)
	{
		net::SendError(id, req, 1006, "unknown command");
		return;
	}

	if ("auth" == strCmd)
	{
		mIter->second(id, req);
		return;
	}

	if (!IsAuthenticated(id))
	{
		net::SendError(id, req, 1002, "authentication required");
		return;
	}

	mIter->second(id, req);
}

bool CMarketService::HandleAuth(net::_TyConnectionId id, const CRequest& req)
{
	std::string strToken = req.GetExtraData("token");
	if (!strToken.empty())
	{
		return HandleReAuth(id, req);
	}

	if ("auth" == req.GetCmd())
	{
		if (Login(id, req, strToken))
		{
			std::lock_guard<std::mutex> lck(m_mtx_sessions);
			m_auth_clients.emplace(id);
			m_client_tokens.insert_or_assign(strToken, std::chrono::steady_clock::now() + TokenLifetime);
		}
		return true;
	}
	net::SendError(id, req, InvalidRequest, "unsupported authentication request");
	return false;
}

bool CMarketService::HandleReAuth(net::_TyConnectionId id, const CRequest& req)
{
	std::string strToken = req.GetExtraData("token");
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
		SendAuthResponse(id, req, 0, "认证成功");
		return true;
	}
	SendAuthResponse(id, req, InvalidCredentials, "登录状态已失效");
	return false;
}

bool CMarketService::Login(net::_TyConnectionId id, const CRequest& req, std::string& strToken)
{
	std::string strAccount = req.GetExtraData("user");
	std::string strPassword = req.GetExtraData("password");
	if (!IsAccountValid(strAccount) || !IsPasswordValid(strPassword))
	{
		SendAuthResponse(id, req, InvalidCredentials, "账号或密码错误");
		return false;
	}

	db::_TyDBPtr db = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == db)
	{
		SendAuthResponse(id, req, StorageUnavailable, "用户数据库暂不可用");
		return false;
	}

	std::string strSql = "SELECT user_id, account FROM table_user WHERE account=" + utility::Utf8Literal(strAccount) + " AND password_hash=UNHEX(SHA2(CONCAT(password_salt,UNHEX('" + utility::ToHex(strPassword) + "')),256)) AND status=1 LIMIT 1";
	const db::CQueryTable& table = db->ExecQuery(strSql);
	if (table.m_rows.empty())
	{
		SendAuthResponse(id, req, InvalidCredentials, "账号或密码错误");
		return false;
	}

	CRequest response;
	response.SetId(req.GetId());
	response.SetType(req.GetType());
	response.SetCmd(req.GetCmd());
	response.SetReturnData("accepted", "1");
	response.SetReturnData("user_id", db::QueryValueToString(table.m_rows.front().at(0)));
	response.SetReturnData("account", db::QueryValueToString(table.m_rows.front().at(1)));
	strToken = utility::MakeSaltHex();
	response.SetReturnData("token", strToken);
	net::SendRequest(id, response);
	return true;
}

void CMarketService::SendAuthResponse(net::_TyConnectionId id, const CRequest& req, int nErrorCode, const std::string& strMessage) const
{
	CRequest response;
	response.SetId(req.GetId());
	response.SetType(req.GetType());
	response.SetCmd(req.GetCmd());
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

bool CMarketService::HandleHeartbeat(net::_TyConnectionId id, const CRequest& req)
{
	std::int64_t clientTime = 0;
	if (!ParseMilliseconds(req.GetExtraData("client_time_ms"), clientTime))
	{
		net::SendError(id, req, 1005, "invalid client_time_ms");
		return false;
	}
	CRequest response;
	response.SetType(req.GetType());
	response.SetId(req.GetId());
	response.SetCmd(req.GetCmd());
	response.SetReturnData("client_time_ms", std::to_string(clientTime));
	response.SetReturnData("request_id", std::to_string(req.GetId()));
	response.SetReturnData("server_time_ms", std::to_string(NowMilliseconds()));
	return net::SendRequest(id, response);
}

bool CMarketService::HandleSubscription(net::_TyConnectionId id, const CRequest& req)
{
	std::string strCmd = req.GetCmd();
	if (("subscribe" != strCmd) && ("unsubscribe" != strCmd))
	{
		net::SendError(id, req, 1001, "invalid cmd");
		return false;
	}

	bool bSubscribe = "subscribe" == strCmd;
	std::uint64_t requestId = req.GetId();
	std::vector<market::CChannelInfo> requested;
	const _TyReqData& message = req.GetData();
	if (bSubscribe && message.has_subscribe_request())
	{
		const hqmarket::market::v1::SubscribeRequest& payload = message.subscribe_request();
		requested.reserve(static_cast<std::size_t>(payload.securities_size()) * static_cast<std::size_t>(payload.channels_size()));
		for (const auto& securityValue : payload.securities())
		{
			market::CSecurity security(securityValue.symbol(), static_cast<market::Exchange>(static_cast<int>(securityValue.exchange())));
			for (hqmarket::market::v1::Channel channelValue : payload.channels())
			{
				market::Channel channel = static_cast<market::Channel>(static_cast<int>(channelValue));
				if (security.IsValid() && IsRealtimeChannel(channel))
				{
					requested.emplace_back(market::CChannelInfo{ security, channel });
				}
			}
		}
	}
	else if (!bSubscribe && message.has_unsubscribe_request())
	{
		const hqmarket::market::v1::UnsubscribeRequest& payload = message.unsubscribe_request();
		requested.reserve(static_cast<std::size_t>(payload.securities_size()) * static_cast<std::size_t>(payload.channels_size()));
		for (const auto& securityValue : payload.securities())
		{
			market::CSecurity security(securityValue.symbol(), static_cast<market::Exchange>(static_cast<int>(securityValue.exchange())));
			for (hqmarket::market::v1::Channel channelValue : payload.channels())
			{
				market::Channel channel = static_cast<market::Channel>(static_cast<int>(channelValue));
				if (security.IsValid() && IsRealtimeChannel(channel))
				{
					requested.emplace_back(market::CChannelInfo{ security, channel });
				}
			}
		}
	}
	else
	{
		market::CSecurity security = ParseInstrument(req.GetExtraData("security"));
		market::Channel channel = ParseChannel(req.GetExtraData("channel"));
		if (security.IsValid() && IsRealtimeChannel(channel))
		{
			requested.emplace_back(market::CChannelInfo{ security, channel });
		}
	}
	if (requested.empty())
	{
		net::SendError(id, req, 1003, "invalid security or unsupported subscription channel");
		return false;
	}
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
	for (const market::CChannelInfo& subscription : requested)
	{
		wire::SubscriptionResult* pResult = ack.add_results();
		pResult->mutable_security()->set_symbol(subscription.m_security.m_strCode);
		pResult->mutable_security()->set_exchange(ToWire(subscription.m_security.m_market));
		pResult->set_channel(static_cast<wire::Channel>(static_cast<int>(subscription.m_channel)));
		pResult->set_accepted(true);
	}

	CRequest response;
	response.SetCmd("subscription_ack");
	response.SetReturnData("accepted", "1");
	SetData(response, ack, requestId);
	net::SendRequest(id, response);
	if (bSubscribe)
	{
		for (const market::CChannelInfo& subscription : requested)
		{
			if (market::Channel::quote != subscription.m_channel)
			{
				continue;
			}
			std::optional<market::CQuote> quote = m_broker.QueryQuote(subscription.m_security);
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
	}
	return true;
}

bool CMarketService::HandleQuery(net::_TyConnectionId id, const CRequest& req)
{
	std::uint64_t requestId = req.GetId();
	std::string strCmd = req.GetCmd();
	if ("query_securities" == strCmd)
	{
		wire::SecurityList result;
		std::vector<market::CInstrument> instruments = m_broker.QueryInstruments();
		if (instruments.empty())
		{
			net::SendError(id, req, StorageUnavailable, "security list is unavailable");
			return false;
		}
		std::uint64_t version = 1469598103934665603ULL;
		for (const market::CInstrument& instrument : instruments)
		{
			wire::SecurityInfo* pInfo = result.add_securities();
			FillSecurityInfo(instrument, pInfo);
			HashString(version, instrument.m_security.m_strCode);
			version = (version ^ static_cast<std::uint64_t>(instrument.m_security.m_market)) * 1099511628211ULL;
			HashString(version, instrument.m_strName);
			HashString(version, instrument.m_strStatus);
			for (const std::string& strAlias : instrument.m_pinyinFullAliases)
			{
				HashString(version, strAlias);
			}
			for (const std::string& strAlias : instrument.m_pinyinShortAliases)
			{
				HashString(version, strAlias);
			}
		}
		result.set_version(static_cast<std::int64_t>(version));
		CRequest response;
		response.SetCmd(strCmd);
		SetData(response, result, requestId);
		return net::SendRequest(id, response);
	}
	if ("query_sectors" == strCmd)
	{
		int nSectorType = 0;
		if (!utility::to_number(req.GetExtraData("sector_type"), nSectorType) || (static_cast<int>(market::SectorType::industry) != nSectorType))
		{
			net::SendError(id, req, 1003, "unsupported sector type");
			return false;
		}
		std::vector<market::CSector> sectors = m_broker.QuerySectors(market::SectorType::industry);
		if (sectors.empty())
		{
			net::SendError(id, req, StorageUnavailable, "sector data is unavailable");
			return false;
		}
		wire::SectorList result;
		result.set_type(wire::SECTOR_TYPE_INDUSTRY);
		result.set_snapshot_time_ms(sectors.front().m_nSnapshotTime);
		result.set_source("akshare");
		for (const auto& sector : sectors)
		{
			FillSector(sector, result.add_sectors());
		}
		CRequest response;
		response.SetCmd(strCmd);
		SetData(response, result, requestId);
		return net::SendRequest(id, response);
	}
	if ("query_sector_constituents" == strCmd)
	{
		int nSectorType = 0;
		std::string strSectorCode = req.GetExtraData("sector_code");
		if (!utility::to_number(req.GetExtraData("sector_type"), nSectorType) || (static_cast<int>(market::SectorType::industry) != nSectorType) || strSectorCode.empty())
		{
			net::SendError(id, req, 1003, "invalid sector constituent request");
			return false;
		}
		market::CSectorConstituents constituents = m_broker.QuerySectorConstituents(market::SectorType::industry, strSectorCode);
		if (constituents.m_securities.empty())
		{
			net::SendError(id, req, StorageUnavailable, "sector constituents are unavailable");
			return false;
		}
		wire::SectorConstituents result;
		result.set_type(wire::SECTOR_TYPE_INDUSTRY);
		FillSector(constituents.m_sector, result.mutable_sector());
		result.set_snapshot_time_ms(constituents.m_nSnapshotTime);
		result.set_source("akshare");
		for (const auto& instrument : constituents.m_securities)
		{
			wire::SecurityInfo* pInfo = result.add_securities();
			FillSecurityInfo(instrument, pInfo);
		}
		CRequest response;
		response.SetCmd(strCmd);
		SetData(response, result, requestId);
		return net::SendRequest(id, response);
	}

	market::CSecurity security = ParseInstrument(req.GetExtraData("security"));
	market::Channel channel = "query_quote" == strCmd ? market::Channel::quote : ParseChannel(req.GetExtraData("channel"));
	if (!security.IsValid() || ((market::Channel::quote != channel) && (market::Channel::bar_1m != channel) && (market::Channel::bar_1d != channel)))
	{
		net::SendError(id, req, 1003, "invalid security or unsupported query channel");
		return false;
	}
	wire::QueryResponse result;
	wire::QueryResponse* pResult = &result;
	pResult->mutable_security()->set_symbol(security.m_strCode);
	pResult->mutable_security()->set_exchange(ToWire(security.m_market));
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
		if (!ParseMilliseconds(req.GetExtraData("begin_time_ms"), begin) || !ParseMilliseconds(req.GetExtraData("end_time_ms"), end))
		{
			net::SetError(response, req, 1004, "invalid query time range");
			net::SendRequest(id, response);
			return false;
		}
		end = 0 < end ? end : NowMilliseconds();
		if ((0 > begin) || (end < begin))
		{
			net::SetError(response, req, 1004, "invalid query time range");
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
	response.SetCmd(strCmd);
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
	pValue->mutable_security()->set_symbol(depth.m_security.m_strCode);
	pValue->mutable_security()->set_exchange(ToWire(depth.m_security.m_market));
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
	std::vector<market::CInstrument> values = m_broker.QueryInstruments();
	std::ostringstream out;
	out << '[';
	bool bFirst = true;
	for (const market::CInstrument& instrument : values)
	{
		if (!bFirst)
		{
			out << ',';
		}
		bFirst = false;
		out << "{\"security\":\"" << instrument.m_security.String() << "\",\"name\":\"" << instrument.m_strName << "\",\"status\":\"" << instrument.m_strStatus << "\"}";
	}
	out << ']';
	return out.str();
}

std::string CMarketService::BarsJson(const std::string& strInstrument, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime)
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
