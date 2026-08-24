#include "request/request.h"
#include "basic/CDistributor.h"
#include "network/CHttpServer.h"
#include "network/CTcpServer.h"
#include "network/netcommon.h"
#include "python/CPythonRuntime.h"
#include "quote/CHQService.h"
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>

static std::unique_ptr<net::CHttpResponseData> Response(int nStatus, std::string strBody, const std::string& strContentType)
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
	if (!net::IsThreadEnable())
	{
		std::cerr << "Failed to enable libevent thread support" << std::endl;
		return -1;
	}
	return 0;
}

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
	auto python = std::make_unique<CPythonRuntime>();
	if (!python->Initialize(root / "runtime" / "python", root / "python"))
	{
		std::cerr << "Python initialization failed: " << python->GetLastError() << '\n';
		return 3;
	}

	std::unique_ptr<net::CTcpServer> pTcpServer = std::make_unique<net::CTcpServer>(9901);
	service::CMarketService service(pTcpServer.get(), python.get());
	if (!service.Initialize(pszToken, root))
	{
		std::cerr << "HQMarket initialization failed\n";
		service.Stop();
		return 3;
	}
	if (pTcpServer->Initialize() != 0)
	{
		std::cerr << "TCP server initialization failed\n";
		service.Stop();
		return 4;
	}
	std::unique_ptr<net::CHttpServer> pHttpServer = std::make_unique<net::CHttpServer>(9902);
	pHttpServer->RegisterHandler(net::HttpMethod::GET, "/health", [&service](const net::CHttpRequest&)
		{
			return Response(200, service.HealthJson(), "application/json; charset=utf-8");
		});
	pHttpServer->RegisterHandler(net::HttpMethod::GET, "/metrics", [&service](const net::CHttpRequest&)
		{
			return Response(200, service.MetricsText(), "text/plain; charset=utf-8");
		});
	pHttpServer->RegisterHandler(net::HttpMethod::GET, "/v1/instruments",
		[&service](const net::CHttpRequest&)
		{
			return Response(200, service.InstrumentsJson(), "application/json; charset=utf-8");
		});
	pHttpServer->RegisterHandler(net::HttpMethod::GET, "/v1/quotes", [&service](const net::CHttpRequest& request)
		{
			std::string instrument = request.GetQuery("instrument");
			return Response(instrument.empty() ? 400 : 200,
				instrument.empty() ? "{\"error\":\"instrument is required\"}" : service.QuoteJson(instrument),
				"application/json; charset=utf-8");
		});
	pHttpServer->RegisterHandler(net::HttpMethod::GET, "/v1/bars", [&service](const net::CHttpRequest& request)
		{
			std::string instrument = request.GetQuery("instrument");
			if (instrument.empty())
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

	std::thread tcpThread([&pTcpServer]()
		{
			if (pTcpServer != nullptr)
			{
				pTcpServer->Start(true);
			}
		});
	pHttpServer->Start(true);
	pTcpServer->ShutDown();
	if (tcpThread.joinable())
	{
		tcpThread.join();
	}
	service.Stop();
	python->Finalize();
	return 0;
}
