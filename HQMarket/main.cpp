#include "network/CHttpServer.h"
#include "network/netcommon.h"
#include "service/CMarketService.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

static std::unique_ptr<net::CHttpResponseData> Response(int nStatus, std::string strBody, const std::string& strContentType)
{
	auto response = std::make_unique<net::CHttpResponseData>(); response->m_nStatus = nStatus; response->m_headers["Content-Type"] = strContentType; response->m_strBody = std::move(strBody); return response;
}

int main()
{
	net::EnvInitialize();
	if (!net::IsThreadEnable()) { std::cerr << "Failed to enable libevent thread support\n"; return 1; }
	const char* pszToken = std::getenv("HQMARKET_TOKEN");
	if ((nullptr == pszToken) || ('\0' == *pszToken)) { std::cerr << "HQMARKET_TOKEN is required\n"; return 2; }
	const char* pszHome = std::getenv("HQMARKET_HOME"); const std::filesystem::path root = pszHome && *pszHome ? pszHome : std::filesystem::current_path();
	service::CMarketService service;
	if (!service.Initialize(9901, pszToken, root)) { std::cerr << "HQMarket initialization failed\n"; return 3; }
	std::thread tcpThread([&service]() { service.Run(); });
	net::CHttpServer http(9902);
	http.RegisterHandler(net::HttpMethod::GET, "/health", [&service](const net::CHttpRequest&) { return Response(200, service.HealthJson(), "application/json; charset=utf-8"); });
	http.RegisterHandler(net::HttpMethod::GET, "/metrics", [&service](const net::CHttpRequest&) { return Response(200, service.MetricsText(), "text/plain; charset=utf-8"); });
	http.RegisterHandler(net::HttpMethod::GET, "/v1/instruments", [&service](const net::CHttpRequest&) { return Response(200, service.InstrumentsJson(), "application/json; charset=utf-8"); });
	http.RegisterHandler(net::HttpMethod::GET, "/v1/quotes", [&service](const net::CHttpRequest& request) { const auto instrument = request.GetQuery("instrument"); return Response(instrument.empty() ? 400 : 200, instrument.empty() ? "{\"error\":\"instrument is required\"}" : service.QuoteJson(instrument), "application/json; charset=utf-8"); });
	http.RegisterHandler(net::HttpMethod::GET, "/v1/bars", [&service](const net::CHttpRequest& request) { const auto instrument = request.GetQuery("instrument"); if (instrument.empty()) return Response(400, "{\"error\":\"instrument is required\"}", "application/json; charset=utf-8"); return Response(200, service.BarsJson(instrument, market::Channel::bar_1d, 0, INT64_MAX), "application/json; charset=utf-8"); });
	if (http.Initialize() != 0) { service.Stop(); tcpThread.join(); return 4; }
	http.Start(true); service.Stop(); if (tcpThread.joinable()) tcpThread.join(); return 0;
}
