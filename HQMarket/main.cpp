#include "network/CHttpServer.h"
#include "network/CTcpServer.h"
#include "network/netcommon.h"
#include "service/CMarketService.h"
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>

static std::unique_ptr<net::CHttpResponseData> Response(int nStatus, std::string strBody,
														const std::string& strContentType)
{
	std::unique_ptr<net::CHttpResponseData> response = std::make_unique<net::CHttpResponseData>();
	response->m_nStatus = nStatus;
	response->m_headers["Content-Type"] = strContentType;
	response->m_strBody = std::move(strBody);
	return response;
}

int BootLoader()
{
	// 必须在网络服务器构造、event_base 创建之前调用。
	net::EnvInitialize();
	if (net::IsThreadEnable() == false)
	{
		std::cerr << "Failed to enable libevent thread support" << std::endl;
		return -1;
	}
	return 0;
}

net::CTcpServer* pTcpServer = nullptr;
net::CHttpServer* pHttpServer = nullptr;

int main()
{
	if (BootLoader() != 0)
	{
		return -1;
	}
	const char* pszToken = std::getenv("HQMARKET_TOKEN");
	if ((pszToken == nullptr) || (*pszToken == '\0'))
	{
		std::cerr << "HQMARKET_TOKEN is required\n";
		return 2;
	}
	const char* pszHome = std::getenv("HQMARKET_HOME");
	std::filesystem::path root = ((pszHome != nullptr) && (*pszHome != '\0')) ? pszHome : std::filesystem::current_path();

	if (nullptr == pTcpServer)
	{
		pTcpServer = new net::CTcpServer(9901);
	}
	service::CMarketService service(pTcpServer);
	if (service.Initialize(pszToken, root) == false)
	{
		std::cerr << "HQMarket initialization failed\n";
		return 3;
	}
	if (pTcpServer->Initialize() != 0)
	{
		std::cerr << "TCP server initialization failed\n";
		service.Stop();
		return 4;
	}
	if (nullptr == pHttpServer)
	{
		pHttpServer = new net::CHttpServer(9902);
	}
	pHttpServer->RegisterHandler(net::HttpMethod::GET, "/health",
		[&service](const net::CHttpRequest&)
		{
			return Response(200, service.HealthJson(), "application/json; charset=utf-8");
		});
	pHttpServer->RegisterHandler(net::HttpMethod::GET, "/metrics",
		[&service](const net::CHttpRequest&)
		{
			return Response(200, service.MetricsText(), "text/plain; charset=utf-8");
		});
	pHttpServer->RegisterHandler(net::HttpMethod::GET, "/v1/instruments",
		[&service](const net::CHttpRequest&)
		{
			return Response(200, service.InstrumentsJson(), "application/json; charset=utf-8");
		});
	pHttpServer->RegisterHandler(net::HttpMethod::GET, "/v1/quotes",
		[&service](const net::CHttpRequest& request)
		{
			std::string instrument = request.GetQuery("instrument");
			return Response(instrument.empty() == true ? 400 : 200,
				instrument.empty() == true ? "{\"error\":\"instrument is required\"}" : service.QuoteJson(instrument),
				"application/json; charset=utf-8");
		});
	pHttpServer->RegisterHandler(net::HttpMethod::GET, "/v1/bars",
		[&service](const net::CHttpRequest& request)
		{
			std::string instrument = request.GetQuery("instrument");
			if (instrument.empty() == true)
			{
				return Response(400, "{\"error\":\"instrument is required\"}",
								"application/json; charset=utf-8");
			}
			return Response(200,
				service.BarsJson(instrument, market::Channel::bar_1d, 0, std::numeric_limits<std::int64_t>::max()),
				"application/json; charset=utf-8");
		});
	if (pHttpServer->Initialize() != 0)
	{
		service.Stop();
		return 5;
	}

	std::thread tcpThread(
		[]()
		{
			if (pTcpServer != nullptr)
			{
				pTcpServer->Start(true);
			}
		});
	pHttpServer->Start(true);
	pTcpServer->ShutDown();
	if (tcpThread.joinable() == true)
	{
		tcpThread.join();
	}
	service.Stop();
	pHttpServer = nullptr;
	pTcpServer = nullptr;
	return 0;
}
