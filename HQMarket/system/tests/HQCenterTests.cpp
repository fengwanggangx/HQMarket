#include "../CHQCenter.h"
#include "../../request/request.pb.h"
#include "../CHQService.h"
#include "../../network/CHttpServer.h"
#include "../../python/CPythonRuntime.h"
#include "../../database/CDBEngine.h"
#include "HQTestBoundaries.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

class CHQRoutingTests
{
	static void Check(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	static CRequest Request(net::_TyConnectionId id, _TyRequest type, std::uint64_t requestId)
	{
		CRequest req;
		req.SetConnectionId(id);
		req.SetType(type);
		req.SetId(requestId);
		req.SetExtraData("security", "600519.SSE");
		req.SetExtraData("channel", "quote");
		return req;
	}

	using _TyHttpHandler = std::function<std::unique_ptr<net::CHttpResponseData>(const net::CHttpRequest&)>;
	static std::unique_ptr<net::CHttpResponseData> Http(_TyHttpHandler&& handler)
	{
		net::CHttpServer server(0);
		server.RegisterHandler(net::HttpMethod::GET, "/offline", std::move(handler));
		return std::move(hqtest::State().m_httpResponse);
	}

public:
	static void Run()
	{
		hqtest::CBoundaryState& state = hqtest::State();
		CPythonRuntime runtime;
		CHQService service(&runtime);
		CHQCenter center(&service);
		Check(center.Initialize(), "center initialization");
		std::filesystem::path root = std::filesystem::current_path() / ("routing-runtime-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Check(service.Initialize(root), "service initialization with offline sources and SQLite");
		market::CChannelInfo quoteSubscription{ { "600519", market::Exchange::sse }, market::Channel::quote };

		center.OnRequest(Request(1, _TyRequest::MARKET_SUBSCRIBE, 10));
		Check(1 == state.m_sent.size() && state.m_sent.back().second.GetErrorInfo().has_value() && 0 == state.m_subscribeCalls, "unauthenticated requests never reach subscription service");
		center.m_client_tokens.emplace("valid", std::chrono::steady_clock::now() + std::chrono::hours(1));
		for (net::_TyConnectionId id : { 1, 2 })
		{
			CRequest auth = Request(id, _TyRequest::AUTH_LOGIN, 20 + id);
			auth.SetExtraData("token", "valid");
			center.OnRequest(auth);
			Check(!state.m_sent.back().second.GetErrorInfo().has_value(), "token authentication preserved");
		}
		CRequest heartbeat = Request(1, _TyRequest::HEARTBEAT, 25);
		heartbeat.SetExtraData("client_time_ms", "123456");
		center.OnRequest(heartbeat);
		Check(25 == state.m_sent.back().second.GetId() && "123456" == state.m_sent.back().second.GetExtraData("client_time_ms"), "heartbeat correlation preserved");
		state.m_sent.clear();
		center.OnRequest(Request(1, _TyRequest::MARKET_SUBSCRIBE, 30));
		center.OnRequest(Request(2, _TyRequest::MARKET_SUBSCRIBE, 31));
		Check(2 == state.m_sent.size() && state.m_sent.front().second.GetData().has_subscription_ack() && 30 == state.m_sent.front().second.GetId(), "subscription ACKs preserve original ids");
		Check(1 == state.m_subscribeCalls && 2 == service.m_subscriptions.GetSubscriptionCount(quoteSubscription), "clients share one upstream subscription");
		state.m_sent.clear();
		market::CQuote quote;
		quote.m_security = quoteSubscription.m_security;
		quote.m_nLastPrice = 123450;
		state.m_quoteHandler(std::move(quote));
		Check(2 == state.m_sent.size() && state.m_sent.front().first != state.m_sent.back().first && 0 == state.m_sent.front().second.GetId() && state.m_sent.front().second.GetData().has_quote(), "source quote travels through broker, center and service to both subscribers");
		Check("1" == state.m_sent.front().second.GetExtraData("sequence"), "quote sequence preserved");
		state.m_sent.clear();
		center.OnRequest(Request(1, _TyRequest::MARKET_QUOTE_QUERY, 40));
		Check(1 == state.m_sent.size() && 40 == state.m_sent.back().second.GetId() && state.m_sent.back().second.GetData().query_response().found(), "query response uses cache and original request id");
		state.m_sent.clear();
		center.OnRequest(Request(1, _TyRequest::MARKET_SUBSCRIBE, 41));
		Check(2 == state.m_sent.size() && state.m_sent.front().second.GetData().has_subscription_ack() && state.m_sent.back().second.GetData().has_quote() && 1 == state.m_subscribeCalls, "cached snapshot follows ACK without duplicate upstream subscribe");
		center.OnRequest(Request(1, _TyRequest::MARKET_UNSUBSCRIBE, 42));
		Check(0 == state.m_unsubscribeCalls, "first client unsubscribe retains other client demand");
		state.m_sent.clear();
		quote.m_security = quoteSubscription.m_security;
		state.m_quoteHandler(std::move(quote));
		Check(1 == state.m_sent.size() && 2 == state.m_sent.front().first, "unsubscribed client receives no quote");
		net::CNetEvent disconnected(net::em_event::disconnected, 2);
		center.OnClientNetEvent(disconnected);
		Check(1 == state.m_unsubscribeCalls && state.m_subscriptions.empty() && 0 == service.m_subscriptions.GetSubscriptionCount(quoteSubscription), "last client disconnect releases source subscription");

		CRequest depthRequest = Request(1, _TyRequest::MARKET_SUBSCRIBE, 50);
		depthRequest.SetExtraData("channel", "depth");
		center.OnRequest(depthRequest);
		state.m_sent.clear();
		market::CDepth depth;
		depth.m_security = quoteSubscription.m_security;
		depth.m_bids.push_back({ 100, 10, 4 });
		state.m_depthHandler(std::move(depth));
		Check(1 == state.m_sent.size() && state.m_sent.front().second.GetData().has_depth() && "1" == state.m_sent.front().second.GetExtraData("sequence"), "depth callback is routed through center with sequence");
		state.m_failedConnection = 1;
		depth.m_security = quoteSubscription.m_security;
		state.m_depthHandler(std::move(depth));
		Check(!center.IsAuthenticated(1) && state.m_closed.contains(1) && state.m_subscriptions.empty(), "failed network send clears authentication and subscriptions");
		state.m_failedConnection = -1;

		center.m_client_tokens.emplace("expired", std::chrono::steady_clock::now() - std::chrono::seconds(1));
		CRequest auth = Request(3, _TyRequest::AUTH_LOGIN, 60);
		auth.SetExtraData("token", "expired");
		center.OnRequest(auth);
		Check(state.m_sent.back().second.GetErrorInfo().has_value() && !center.IsAuthenticated(3), "expired token rejected");
		std::unique_ptr<net::CHttpResponseData> response = Http(std::bind_front(&CHQCenter::OnQuoteRequest, &center));
		Check(400 == response->m_nStatus, "HTTP instrument validation preserved");
		state.m_query.emplace("instrument", "600519.SSE");
		response = Http(std::bind_front(&CHQCenter::OnQuoteRequest, &center));
		Check(200 == response->m_nStatus && std::string::npos != response->m_strBody.find("600519.SSE"), "HTTP quote routes to service cache");
		response = Http(std::bind_front(&CHQCenter::OnBarsRequest, &center));
		Check(200 == response->m_nStatus && std::string::npos != response->m_strBody.find("123450"), "HTTP bars route through history and SQLite recorder");
		response = Http(std::bind_front(&CHQCenter::OnInstrumentsRequest, &center));
		Check(std::string::npos != response->m_strBody.find("Sample"), "HTTP instrument list preserved");
		response = Http(std::bind_front(&CHQCenter::OnHealthRequest, &center));
		Check(std::string::npos != response->m_strBody.find("\"status\":\"ok\""), "HTTP health reflects provider state");
		response = Http(std::bind_front(&CHQCenter::OnMetricsRequest, &center));
		Check(std::string::npos != response->m_strBody.find("hqmarket_clients 123"), "network metric supplied by center");

		service.Stop();
		center.Stop();
		Check(nullptr == state.m_quoteHandler && nullptr == state.m_depthHandler && center.m_auth_clients.empty(), "stop drains source routes before detaching center");
		CDBEngine::InstanceRef().Close();
	}
};

int main()
{
	try
	{
		CHQRoutingTests::Run();
		std::cout << "HQMarket center/service routing tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
