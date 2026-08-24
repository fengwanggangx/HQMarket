#include "request/request.h"
#include "basic/CDistributor.h"
#include "network/CHttpServer.h"
#include "quote/CHQService.h"
#include "system/CBootLoader.h"
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>

static std::unique_ptr<net::CHttpResponseData> Response(int nStatus, std::string strBody, const std::string& strContentType)
{
	std::unique_ptr<net::CHttpResponseData> response = std::make_unique<net::CHttpResponseData>();
	response->m_nStatus = nStatus;
	response->m_headers["Content-Type"] = strContentType;
	response->m_strBody = std::move(strBody);
	return response;
}

int main()
{
	CBootLoader boot;
	if (!boot.Initialize())
	{
		std::cerr << boot.GetLastError() << '\n';
		return boot.GetErrorCode();
	}

	net::CTcpServer& tcpServer = boot.GetTcpServer();
	net::CHttpServer& httpServer = boot.GetHttpServer();
	service::CMarketService service(&tcpServer, &boot.GetPythonRuntime());
	if (!service.Initialize(boot.GetToken(), boot.GetRoot()))
	{
		std::cerr << "HQMarket initialization failed\n";
		service.Stop();
		return 3;
	}
	httpServer.RegisterHandler(net::HttpMethod::GET, "/health", [&service](const net::CHttpRequest&)
		{
			return Response(200, service.HealthJson(), "application/json; charset=utf-8");
		});
	httpServer.RegisterHandler(net::HttpMethod::GET, "/metrics", [&service](const net::CHttpRequest&)
		{
			return Response(200, service.MetricsText(), "text/plain; charset=utf-8");
		});
	httpServer.RegisterHandler(net::HttpMethod::GET, "/v1/instruments", [&service](const net::CHttpRequest&)
		{
			return Response(200, service.InstrumentsJson(), "application/json; charset=utf-8");
		});
	httpServer.RegisterHandler(net::HttpMethod::GET, "/v1/quotes", [&service](const net::CHttpRequest& request)
		{
			std::string instrument = request.GetQuery("instrument");
			return Response(instrument.empty() ? 400 : 200, instrument.empty() ? "{\"error\":\"instrument is required\"}" : service.QuoteJson(instrument), "application/json; charset=utf-8");
		});
	httpServer.RegisterHandler(net::HttpMethod::GET, "/v1/bars", [&service](const net::CHttpRequest& request)
		{
			std::string instrument = request.GetQuery("instrument");
			if (instrument.empty())
			{
				return Response(400, "{\"error\":\"instrument is required\"}", "application/json; charset=utf-8");
			}
			return Response(200, service.BarsJson(instrument, market::Channel::bar_1d, 0, std::numeric_limits<std::int64_t>::max()), "application/json; charset=utf-8");
		});
	if (!boot.Run())
	{
		std::cerr << boot.GetLastError() << '\n';
		service.Stop();
		return boot.GetErrorCode();
	}
	service.Stop();
	boot.Finalize();
	return 0;
}
