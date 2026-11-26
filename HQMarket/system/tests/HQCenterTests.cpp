#include "../CHQCenter.h"
#include "../../request/request.pb.h"
#include "../CHQService.h"
#include "../../network/CHttpServer.h"
#include "../../python/CPythonRuntime.h"
#include "../../database/CDBEngine.h"
#include "HQTestBoundaries.h"
#include <filesystem>
#include <iostream>
#include <limits>
#include <thread>
#include <rapidjson/document.h>
#include "../../database/IDataBase.h"
#include "../../quote/CHQRecorder.h"
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
		return std::move(hqtest::State().m_http_response);
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
		std::filesystem::create_directories(root / "data");
		db::CConnectParam params("", 0, "", "", (root / "data" / "hqmarket.db").string(), "");
		Check(0 == CDBEngine::InstancePtr()->Initialize(db::em_database::sqlite, params), "open legacy SQLite fixture");
		{
			db::_TyDBPtr sqlite = CDBEngine::InstancePtr()->GetDBPtr(db::em_database::sqlite);
			Check(0 == sqlite->ExecUpdate("CREATE TABLE bar(symbol TEXT, exchange INTEGER, channel INTEGER, begin_time INTEGER, open INTEGER, high INTEGER, low INTEGER, close INTEGER, volume INTEGER, turnover INTEGER, price_scale INTEGER, adjustment TEXT, source TEXT, PRIMARY KEY(symbol,exchange,channel,begin_time,adjustment))"), "create legacy bars table");
			CHQRecorder recorder;
			Check(recorder.Open(root / "data" / "hqmarket.db"), "migrate legacy bars table");
			market::CBar bar;
			bar.m_security = { "600519", market::Exchange::sse };
			bar.m_channel = market::Channel::bar_1m;
			bar.m_begin_time = 1000;
			bar.m_end_time = 61000;
			bar.m_completed = true;
			bar.m_data_version = std::numeric_limits<std::uint64_t>::max();
			bar.m_basis_version = "basis'1";
			bar.m_turnover = 92500;
			Check(recorder.UpsertBars({ bar }), "persist bar metadata");
			CHQRecorder::CMarketSnapshot snapshot;
			snapshot.m_version = 42;
			snapshot.m_expected = 1;
			snapshot.m_finished_at = 2000;
			snapshot.m_directory_verified = true;
			market::CQuote quote;
			quote.m_security = bar.m_security;
			quote.m_source = "source'1";
			quote.m_last_price = 20000;
			quote.m_pre_close = 18000;
			snapshot.m_quotes.push_back(quote);
			Check(recorder.SaveMarketSnapshot(snapshot), "persist market snapshot");
			Check(recorder.SaveMarketSnapshot(snapshot, false, true), "persist complete mainland market snapshot");
			snapshot.m_version = 43;
			snapshot.m_quotes.front().m_security = { "00700", market::Exchange::hkex };
			market::CInstrument instrument;
			instrument.m_security = snapshot.m_quotes.front().m_security;
			instrument.m_kind = market::InstrumentKind::hkStock;
			instrument.m_status = "halted";
			snapshot.m_instruments.push_back(instrument);
			Check(recorder.SaveMarketSnapshot(snapshot, true), "persist Hong Kong snapshot independently");
			recorder.Close();
			Check(recorder.Open(root / "data" / "hqmarket.db"), "reopen migrated bars table");
			std::optional<CHQRecorder::CMarketSnapshot> restored = recorder.LoadMarketSnapshot();
			std::optional<CHQRecorder::CMarketSnapshot> complete = recorder.LoadMarketSnapshot(false, true);
			Check(complete.has_value() && 42 == complete->m_version && complete->m_directory_verified,
				"complete market snapshot survives restart independently of latest snapshots");
			Check(restored.has_value() && 1 == restored->m_quotes.size() && 18000 == restored->m_quotes.front().m_pre_close,
				"snapshot restores the reference previous close used by rankings");
			std::optional<CHQRecorder::CMarketSnapshot> restoredHongKong = recorder.LoadMarketSnapshot(true);
			Check(restoredHongKong.has_value() && 1 == restoredHongKong->m_instruments.size()
				&& market::InstrumentKind::hkStock == restoredHongKong->m_instruments.front().m_kind
				&& "halted" == restoredHongKong->m_instruments.front().m_status, "snapshot restores stock classification and halt status");
			Check(restoredHongKong.has_value() && 43 == restoredHongKong->m_version && 1 == restoredHongKong->m_quotes.size() && market::Exchange::hkex == restoredHongKong->m_quotes.front().m_security.m_market,
				"Hong Kong snapshot restores separately from the mainland snapshot");
			Check(restored.has_value() && restored->m_directory_verified && 42 == restored->m_version && 1 == restored->m_quotes.size() && quote.m_source == restored->m_quotes.front().m_source && quote.m_last_price == restored->m_quotes.front().m_last_price,
				"market snapshot survives SQLite round trip");
			std::vector<market::CBar> bars = recorder.QueryBars(bar.m_security, bar.m_channel, 1000, 1000);
			Check(1 == bars.size() && bars.front().m_completed && bars.front().m_end_time == bar.m_end_time &&
					  bars.front().m_data_version == bar.m_data_version && bars.front().m_basis_version == bar.m_basis_version,
				"metadata survives SQLite round trip");
			bar.m_hasTurnover = false;
			bar.m_turnover = 0;
			Check(recorder.UpsertBars({ bar }), "refresh bar without turnover");
			bars = recorder.QueryBars(bar.m_security, bar.m_channel, 1000, 1000);
			Check(1 == bars.size() && 92500 == bars.front().m_turnover, "missing turnover preserves stored amount");
			bar.m_hasTurnover = true;
			Check(recorder.UpsertBars({ bar }), "refresh bar with known zero turnover");
			bars = recorder.QueryBars(bar.m_security, bar.m_channel, 1000, 1000);
			Check(1 == bars.size() && 0 == bars.front().m_turnover, "known zero turnover replaces stored amount");
		}
		Check(service.Initialize(root), "service initialization with offline sources and SQLite");
		market::CChannelInfo quoteSubscription{ { "600519", market::Exchange::sse }, market::Channel::quote };

		center.OnRequest(Request(1, _TyRequest::MARKET_SUBSCRIBE, 10));
		Check(1 == state.m_sent.size() && state.m_sent.back().second.GetErrorInfo().has_value() && 0 == state.m_subscribe_calls, "unauthenticated requests never reach subscription service");
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
		Check(1 == state.m_subscribe_calls && 2 == service.m_subscriptions.GetSubscriptionCount(quoteSubscription), "clients share one upstream subscription");
		state.m_sent.clear();
		market::CQuote quote;
		quote.m_security = quoteSubscription.m_security;
		quote.m_last_price = 123450;
		state.m_quote_handler(std::move(quote));
		Check(2 == state.m_sent.size() && state.m_sent.front().first != state.m_sent.back().first && 0 == state.m_sent.front().second.GetId() && state.m_sent.front().second.GetData().has_quote(), "source quote travels through broker, center and service to both subscribers");
		Check("1" == state.m_sent.front().second.GetExtraData("sequence"), "quote sequence preserved");
		state.m_sent.clear();
		center.OnRequest(Request(1, _TyRequest::MARKET_QUOTE_QUERY, 40));
		Check(1 == state.m_sent.size() && 40 == state.m_sent.back().second.GetId() && state.m_sent.back().second.GetData().query_response().found(), "query response uses cache and original request id");
		state.m_sent.clear();
		request::RequestData snapshotQuery;
		snapshotQuery.set_type(request::MARKET_QUOTE_QUERY);
		snapshotQuery.set_id(140);
		snapshotQuery.mutable_market_snapshot_query()->set_scope("hk");
		CRequest snapshotRequest;
		Check(snapshotRequest.Deserialize(snapshotQuery.SerializeAsString()), "decode typed market snapshot request");
		snapshotRequest.SetConnectionId(1);
		center.OnRequest(snapshotRequest);
		Check(1 == state.m_sent.size() && 140 == state.m_sent.back().second.GetId()
			&& "preparing" == state.m_sent.back().second.GetData().market_snapshot_page().status(),
			"typed market snapshot request routes through the center and returns a typed page");
		state.m_sent.clear();
		center.OnRequest(Request(1, _TyRequest::MARKET_SUBSCRIBE, 41));
		Check(2 == state.m_sent.size() && state.m_sent.front().second.GetData().has_subscription_ack() && state.m_sent.back().second.GetData().has_quote() && 1 == state.m_subscribe_calls, "cached snapshot follows ACK without duplicate upstream subscribe");
		Check("1" == state.m_sent.back().second.GetExtraData("sequence"), "cached snapshot preserves the source sequence");
		state.m_sent.clear();
		CRequest oversized = Request(2, _TyRequest::MARKET_QUOTE_QUERY, 99);
		oversized.SetExtraData("oversized_fixture", std::string(8 * 1024 * 1024, 'x'));
		Check(center.SendRequest(2, oversized), "oversized response sends an error frame");
		Check(1 == state.m_sent.size() && 99 == state.m_sent.back().second.GetId() &&
				  state.m_sent.back().second.GetErrorInfo().has_value() && center.IsAuthenticated(2) &&
				  !state.m_closed.contains(2),
			"oversized response preserves the authenticated connection");
		center.OnRequest(Request(1, _TyRequest::MARKET_UNSUBSCRIBE, 42));
		Check(0 == state.m_unsubscribe_calls, "first client unsubscribe retains other client demand");
		state.m_sent.clear();
		quote.m_security = quoteSubscription.m_security;
		state.m_quote_handler(std::move(quote));
		Check(1 == state.m_sent.size() && 2 == state.m_sent.front().first, "unsubscribed client receives no quote");
		net::CNetEvent disconnected(net::em_event::disconnected, 2);
		center.OnClientNetEvent(disconnected);
		Check(1 == state.m_unsubscribe_calls && state.m_subscriptions.empty() && 0 == service.m_subscriptions.GetSubscriptionCount(quoteSubscription), "last client disconnect releases source subscription");

		CRequest depthRequest = Request(1, _TyRequest::MARKET_SUBSCRIBE, 50);
		depthRequest.SetExtraData("channel", "depth");
		center.OnRequest(depthRequest);
		state.m_sent.clear();
		market::CDepth depth;
		depth.m_security = quoteSubscription.m_security;
		depth.m_bids.push_back({ 100, 10, 4 });
		state.m_depth_handler(std::move(depth));
		Check(1 == state.m_sent.size() && state.m_sent.front().second.GetData().has_depth() && "1" == state.m_sent.front().second.GetExtraData("sequence"), "depth callback is routed through center with sequence");
		state.m_failed_connection = 1;
		depth.m_security = quoteSubscription.m_security;
		state.m_depth_handler(std::move(depth));
		Check(!center.IsAuthenticated(1) && state.m_closed.contains(1) && state.m_subscriptions.empty(), "failed network send clears authentication and subscriptions");
		state.m_failed_connection = -1;

		center.m_client_tokens.emplace("expired", std::chrono::steady_clock::now() - std::chrono::seconds(1));
		CRequest auth = Request(3, _TyRequest::AUTH_LOGIN, 60);
		auth.SetExtraData("token", "expired");
		center.OnRequest(auth);
		Check(state.m_sent.back().second.GetErrorInfo().has_value() && !center.IsAuthenticated(3), "expired token rejected");
		std::unique_ptr<net::CHttpResponseData> response = Http(std::bind_front(&CHQCenter::OnQuoteRequest, &center));
		Check(400 == response->m_status, "HTTP instrument validation preserved");
		response = Http(std::bind_front(&CHQCenter::OnMarketSnapshotRequest, &center));
		Check(200 == response->m_status && std::string::npos != response->m_body.find("preparing"), "market snapshot reports preparing before publication");
		state.m_query["offset"] = "1";
		response = Http(std::bind_front(&CHQCenter::OnMarketSnapshotRequest, &center));
		Check(400 == response->m_status, "market snapshot continuation requires a version");
		state.m_query.erase("offset");
		state.m_query.emplace("instrument", "600519.SSE");
		response = Http(std::bind_front(&CHQCenter::OnQuoteRequest, &center));
		Check(200 == response->m_status && std::string::npos != response->m_body.find("600519.SSE"), "HTTP quote routes to service cache");
		response = Http(std::bind_front(&CHQCenter::OnBarsRequest, &center));
		Check(200 == response->m_status && std::string::npos != response->m_body.find("123450"), "HTTP bars route through history and SQLite recorder");
		std::size_t historyCalls = state.m_historyCalls;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		response = Http(std::bind_front(&CHQCenter::OnBarsRequest, &center));
		Check(200 == response->m_status && historyCalls == state.m_historyCalls, "HTTP default end time reuses fresh history");
		state.m_query["channel"] = "bar_1m";
		state.m_query["begin_time_ms"] = "1700000000000";
		state.m_query["end_time_ms"] = "1700000000000";
		response = Http(std::bind_front(&CHQCenter::OnBarsRequest, &center));
		Check(200 == response->m_status && std::string::npos != response->m_body.find("123450"), "HTTP minute bars accept an explicit time range");
		Check("unverified" == response->m_headers.at("X-HQMarket-History-Coverage") &&
				  "1700000000000" == response->m_headers.at("X-HQMarket-First-Bar-Time-Ms") &&
				  "1700000000000" == response->m_headers.at("X-HQMarket-Last-Bar-Time-Ms"),
			"HTTP minute bars expose returned bounds");
		state.m_query["end_time_ms"] = "1700000000001";
		state.m_query["begin_time_ms"] = "1700000000001";
		response = Http(std::bind_front(&CHQCenter::OnBarsRequest, &center));
		Check(200 == response->m_status && "[]" == response->m_body, "HTTP empty range succeeds");
		state.m_query["end_time_ms"] = "invalid";
		response = Http(std::bind_front(&CHQCenter::OnBarsRequest, &center));
		Check(400 == response->m_status, "HTTP rejects invalid time");
		state.m_query["end_time_ms"] = "1700000000001";
		state.m_query["instrument"] = "600000.SSE";
		response = Http(std::bind_front(&CHQCenter::OnBarsRequest, &center));
		Check(503 == response->m_status, "HTTP reports an unavailable upstream");
		response = Http(std::bind_front(&CHQCenter::OnInstrumentsRequest, &center));
		rapidjson::Document instruments;
		instruments.Parse(response->m_body.c_str());
		Check(instruments.IsArray() && 1 == instruments.Size() &&
				  std::string("Sample \"name\" \\ exchange") == instruments[0]["name"].GetString(),
			"HTTP instruments preserve escaped names");
		response = Http(std::bind_front(&CHQCenter::OnHealthRequest, &center));
		Check(std::string::npos != response->m_body.find("\"status\":\"ok\""), "HTTP health reflects provider state");
		response = Http(std::bind_front(&CHQCenter::OnMetricsRequest, &center));
		Check(std::string::npos != response->m_body.find("hqmarket_clients 123"), "network metric supplied by center");
		Check(std::string::npos != response->m_body.find("hqmarket_snapshot_available{market=\"CN\"}")
			&& std::string::npos != response->m_body.find("hqmarket_snapshot_available{market=\"HK\"}"),
			"snapshot availability metrics distinguish mainland and Hong Kong markets");

		service.Stop();
		center.Stop();
		Check(nullptr == state.m_quote_handler && nullptr == state.m_depth_handler && center.m_auth_clients.empty(), "stop drains source routes before detaching center");
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
