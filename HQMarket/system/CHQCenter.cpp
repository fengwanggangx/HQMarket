#include "CHQCenter.h"
#include "CHQService.h"
#include "../common/utility.h"
#include "../database/CDBEngine.h"
#include "../database/IDataBase.h"
#include "../network/CHttpServer.h"
#include "../network/CNetPool.h"
#include "../network/CFrameBuffer.h"
#include "../network/CNetTools.h"
#include <cctype>
#include <exception>
#include <limits>
#include <utility>

namespace
{
	constexpr int InvalidRequest = 1001;
	constexpr int InvalidCredentials = 1002;
	constexpr int StorageUnavailable = 1004;
	constexpr std::size_t MinUserLength = 3;
	constexpr std::size_t MaxUserLength = 64;
	constexpr std::size_t MinPasswordLength = 6;
	constexpr std::size_t MaxPasswordLength = 128;
	constexpr std::chrono::hours TokenLifetime{ 24 };

	bool IsUserValid(const std::string& strUser)
	{
		if ((MinUserLength > strUser.size()) || (MaxUserLength < strUser.size()))
		{
			return false;
		}
		for (unsigned char character : strUser)
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

	std::unique_ptr<net::CHttpResponseData> Response(int nStatus, std::string strBody, const std::string& strContentType)
	{
		std::unique_ptr<net::CHttpResponseData> response = std::make_unique<net::CHttpResponseData>();
		response->m_status = nStatus;
		response->m_headers["Content-Type"] = strContentType;
		response->m_body = std::move(strBody);
		return response;
	}
} // namespace

CHQCenter::CHQCenter(CHQService* pHQService) : m_hq_service(pHQService)
{
	m_handler = {
		{ _TyRequest::AUTH_LOGIN, std::bind_front(&CHQCenter::HandleAuth, this) },
		{ _TyRequest::HEARTBEAT, std::bind_front(&CHQCenter::HandleHeartbeat, this) },
		{ _TyRequest::MARKET_QUOTE_QUERY, std::bind_front(&CHQCenter::HandleMarketRequest, this) },
		{ _TyRequest::MARKET_BARS_QUERY, std::bind_front(&CHQCenter::HandleMarketRequest, this) },
		{ _TyRequest::MARKET_SECURITIES_QUERY, std::bind_front(&CHQCenter::HandleMarketRequest, this) },
		{ _TyRequest::MARKET_SECTORS_QUERY, std::bind_front(&CHQCenter::HandleMarketRequest, this) },
		{ _TyRequest::MARKET_SECTOR_CONSTITUENTS_QUERY, std::bind_front(&CHQCenter::HandleMarketRequest, this) },
		{ _TyRequest::MARKET_SUBSCRIBE, std::bind_front(&CHQCenter::HandleMarketRequest, this) },
		{ _TyRequest::MARKET_UNSUBSCRIBE, std::bind_front(&CHQCenter::HandleMarketRequest, this) }
	};
}

CHQCenter::~CHQCenter()
{
	Stop();
}

bool CHQCenter::Initialize()
{
	if (m_initialized)
	{
		return true;
	}
	if (nullptr == m_hq_service)
	{
		return false;
	}
	m_hq_service->SetReplyHandler(std::bind_front(&CHQCenter::SendRequest, this));
	m_hq_service->SetBroadcastHandler(std::bind_front(&CHQCenter::Broadcast, this));
	m_hq_service->SetQuoteHandler(std::bind_front(&CHQCenter::OnQuote, this));
	m_hq_service->SetDepthHandler(std::bind_front(&CHQCenter::OnDepth, this));
	m_initialized = true;
	return true;
}

void CHQCenter::Stop()
{
	// BootLoader drains network requests and provider callbacks before detaching the center.
	if (nullptr != m_hq_service && m_initialized)
	{
		m_hq_service->SetReplyHandler({});
		m_hq_service->SetBroadcastHandler({});
		m_hq_service->SetQuoteHandler({});
		m_hq_service->SetDepthHandler({});
	}
	std::lock_guard<std::mutex> lock(m_sessions_mutex);
	m_auth_clients.clear();
	m_client_tokens.clear();
	m_initialized = false;
}

int CHQCenter::OnClientNetEvent(const net::CNetEvent& ev)
{
	if (net::em_event::request == ev.m_event && nullptr != ev.m_request)
	{
		OnRequest(*ev.m_request);
	}
	else if (net::em_event::disconnected == ev.m_event || net::em_event::error == ev.m_event || net::em_event::timeout == ev.m_event)
	{
		HandleClientDisconnected(ev.m_connection_id);
	}
	return 1;
}

void CHQCenter::OnRequest(const CRequest& req)
{
	net::_TyConnectionId id = req.GetConnectionId();
	_TyRequest type = req.GetType();
	const auto mIter = m_handler.find(type);
	if (m_handler.end() == mIter)
	{
		net::SendError(id, req, 1006, "unknown request type");
		return;
	}

	if (_TyRequest::AUTH_LOGIN == type)
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

bool CHQCenter::HandleAuth(net::_TyConnectionId id, const CRequest& req)
{
	std::string strToken = req.GetExtraData("token");
	if (!strToken.empty())
	{
		return HandleReAuth(id, req);
	}

	if (_TyRequest::AUTH_LOGIN == req.GetType())
	{
		Login(id, req, strToken);
		return true;
	}
	net::SendError(id, req, InvalidRequest, "unsupported authentication request");
	return false;
}

bool CHQCenter::HandleReAuth(net::_TyConnectionId id, const CRequest& req)
{
	std::string strToken = req.GetExtraData("token");
	bool bAccepted = false;
	{
		std::lock_guard<std::mutex> lck(m_sessions_mutex);
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

bool CHQCenter::Login(net::_TyConnectionId id, const CRequest& req, std::string& strToken)
{
	std::string strUser = req.GetExtraData("user");
	std::string strPassword = req.GetExtraData("password");
	if (!IsUserValid(strUser) || !IsPasswordValid(strPassword))
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

	std::string strSql = "SELECT user_id, user FROM table_user WHERE user=" + utility::Utf8Literal(strUser) + " AND password_hash=UNHEX(SHA2(CONCAT(password_salt,UNHEX('" + utility::ToHex(strPassword) + "')),256)) AND status=1 LIMIT 1";
	const db::CQueryTable& table = db->ExecQuery(strSql);
	if (table.m_rows.empty())
	{
		SendAuthResponse(id, req, InvalidCredentials, "账号或密码错误");
		return false;
	}

	CRequest response;
	response.SetId(req.GetId());
	response.SetType(req.GetType());
	response.SetErrorInfo(0, "");
	response.SetExtraData("user_id", db::QueryValueToString(table.m_rows.front().at(0)));
	response.SetExtraData("user", db::QueryValueToString(table.m_rows.front().at(1)));
	strToken = utility::MakeSaltHex();
	response.SetExtraData("token", strToken);
	{
		std::lock_guard<std::mutex> lck(m_sessions_mutex);
		m_auth_clients.emplace(id);
		m_client_tokens.insert_or_assign(strToken, std::chrono::steady_clock::now() + TokenLifetime);
	}
	net::SendRequest(id, response);
	return true;
}

void CHQCenter::SendAuthResponse(net::_TyConnectionId id, const CRequest& req, int nErrorCode, const std::string& strMessage) const
{
	CRequest response;
	response.SetId(req.GetId());
	response.SetType(req.GetType());
	if (0 != nErrorCode)
	{
		net::SetError(response, nErrorCode, strMessage);
	}
	else
	{

		response.SetErrorInfo(0, "");
	}
	net::SendRequest(id, response);
}

bool CHQCenter::HandleHeartbeat(net::_TyConnectionId id, const CRequest& req)
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
	response.SetType(req.GetType());
	response.SetExtraData("client_time_ms", std::to_string(clientTime));
	response.SetExtraData("request_id", std::to_string(req.GetId()));
	response.SetExtraData("server_time_ms", std::to_string(NowMilliseconds()));
	return net::SendRequest(id, response);
}

bool CHQCenter::IsAuthenticated(net::_TyConnectionId id) const
{
	std::lock_guard<std::mutex> lck(m_sessions_mutex);
	const auto mIter = m_auth_clients.find(id);
	return (m_auth_clients.end() != mIter);
}

bool CHQCenter::HandleMarketRequest(net::_TyConnectionId, const CRequest& req)
{
	return m_hq_service->OnRequest(req);
}

void CHQCenter::HandleClientDisconnected(net::_TyConnectionId id)
{
	net::CNetPool::InstancePtr()->CloseAConnection(id);
	{
		std::lock_guard<std::mutex> lock(m_sessions_mutex);
		m_auth_clients.erase(id);
	}
	m_hq_service->HandleClientDisconnected(id);
}

void CHQCenter::Broadcast(const std::vector<net::_TyConnectionId>& ids, const CRequest& req)
{
	std::string payload;
	if (ids.empty() || !req.Serialize(&payload))
	{
		return;
	}
	std::optional<std::string> frame = net::CFrameBuffer::Encode(payload);
	if (!frame.has_value())
	{
		return;
	}
	for (net::_TyConnectionId id : ids)
	{
		if (IsAuthenticated(id) && !net::CNetPool::InstancePtr()->Send(id, frame->data(), frame->size()))
		{
			HandleClientDisconnected(id);
		}
	}
}

bool CHQCenter::SendRequest(net::_TyConnectionId id, const CRequest& req)
{
	if (!IsAuthenticated(id))
	{
		return false;
	}
	std::string payload;
	if (!req.Serialize(&payload))
	{
		return false;
	}
	std::optional<std::string> frame = net::CFrameBuffer::Encode(payload);
	if (!frame.has_value())
	{
		CRequest error;
		error.SetId(req.GetId());
		error.SetType(req.GetType());
		error.SetErrorInfo(1005, "response exceeds the TCP frame limit");
		return SendRequest(id, error);
	}
	if (!net::CNetPool::InstancePtr()->Send(id, frame->data(), frame->size()))
	{
		HandleClientDisconnected(id);
		return false;
	}
	return true;
}

void CHQCenter::OnDepth(market::CDepth&& depth)
{
	m_hq_service->OnDepth(std::move(depth));
}

void CHQCenter::OnQuote(const market::CQuote& quote, std::uint64_t nSequence)
{
	m_hq_service->OnQuote(quote, nSequence);
}

std::unique_ptr<net::CHttpResponseData> CHQCenter::OnHealthRequest(const net::CHttpRequest&)
{
	return Response(200, m_hq_service->HealthJson(), "application/json; charset=utf-8");
}

std::unique_ptr<net::CHttpResponseData> CHQCenter::OnMetricsRequest(const net::CHttpRequest&)
{
	return Response(200, m_hq_service->MetricsText(net::CNetPool::InstancePtr()->Count()) + "hqmarket_tcp_send_queue_overflows_total " + std::to_string(net::CNetPool::QueueOverflows()) + "\n", "text/plain; charset=utf-8");
}

std::unique_ptr<net::CHttpResponseData> CHQCenter::OnInstrumentsRequest(const net::CHttpRequest&)
{
	return Response(200, m_hq_service->InstrumentsJson(), "application/json; charset=utf-8");
}

std::unique_ptr<net::CHttpResponseData> CHQCenter::OnQuoteRequest(const net::CHttpRequest& req)
{
	std::string instrument = req.GetQuery("instrument");
	return Response(instrument.empty() ? 400 : 200, instrument.empty() ? "{\"error\":\"instrument is required\"}" : m_hq_service->QuoteJson(instrument), "application/json; charset=utf-8");
}

std::unique_ptr<net::CHttpResponseData> CHQCenter::OnBarsRequest(const net::CHttpRequest& req)
{
	std::string instrument = req.GetQuery("instrument");
	if (instrument.empty())
	{
		return Response(400, "{\"error\":\"instrument is required\"}", "application/json; charset=utf-8");
	}
	int status = 200;
	std::string channel = req.GetQuery("channel");
	std::optional<std::pair<std::int64_t, std::int64_t>> returnedRange;
	std::string body = m_hq_service->BarsJson(instrument, channel,
		req.GetQuery("begin_time_ms"), req.GetQuery("end_time_ms"), status, returnedRange);
	std::unique_ptr<net::CHttpResponseData> response = Response(status, std::move(body), "application/json; charset=utf-8");
	if (200 == status && "bar_1m" == channel)
	{
		response->m_headers["X-HQMarket-History-Coverage"] = "unverified";
		if (returnedRange.has_value())
		{
			response->m_headers["X-HQMarket-First-Bar-Time-Ms"] = std::to_string(returnedRange->first);
			response->m_headers["X-HQMarket-Last-Bar-Time-Ms"] = std::to_string(returnedRange->second);
		}
	}
	return response;
}
