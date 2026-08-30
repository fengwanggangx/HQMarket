#include "CBootLoader.h"
#include "../network/CHttpServer.h"
#include "../network/CTcpServer.h"
#include "../python/CPythonRuntime.h"
#include "../ini/CINIHandler.h"

#include <cstdlib>
#include <utility>

namespace net
{
	void EnvInitialize();
	bool IsThreadEnable();
} // namespace net

CBootLoader::CBootLoader() = default;

CBootLoader::~CBootLoader()
{
	Finalize();
}

bool CBootLoader::Initialize()
{
	if (m_bInitialized)
	{
		return true;
	}

	m_nErrorCode = 0;
	m_strLastError.clear();
	net::EnvInitialize();
	if (!net::IsThreadEnable())
	{
		m_nErrorCode = 1;
		m_strLastError = "Failed to enable libevent thread support";
		return false;
	}

	m_strToken = ini::CINIHandler::InstanceRef().GetValue(ini::Config::System, "HQMarket", "token", std::string());
	if (m_strToken.empty())
	{
		m_nErrorCode = 2;
		m_strLastError = "HQMarket token is required in ini/system.ini";
		return false;
	}

	std::string strHome = ini::CINIHandler::InstanceRef().GetValue(ini::Config::System, "HQMarket", "home", std::string());
	if (!strHome.empty())
	{
		m_root = std::move(strHome);
	}
	else
	{
		m_root = std::filesystem::current_path();
	}

	m_pPython = std::make_unique<CPythonRuntime>();
	std::filesystem::path run = m_root / "runtime" / "python";
	std::filesystem::path script = m_root / "python";
	if (!m_pPython->Initialize(run, script))
	{
		m_nErrorCode = 3;
		m_strLastError = "Python initialization failed: " + m_pPython->GetLastError();
		m_pPython.reset();
		return false;
	}
	m_pTcpServer = std::make_unique<net::CTcpServer>(9901);
	m_pHttpServer = std::make_unique<net::CHttpServer>(9902);

	m_bInitialized = true;
	return true;
}

bool CBootLoader::Run()
{
	if (!m_bInitialized || (nullptr == m_pTcpServer) || (nullptr == m_pHttpServer))
	{
		m_nErrorCode = 4;
		m_strLastError = "Boot loader is not initialized";
		return false;
	}
	if (0 != m_pTcpServer->Initialize())
	{
		m_nErrorCode = 4;
		m_strLastError = "TCP server initialization failed";
		return false;
	}
	if (0 != m_pHttpServer->Initialize())
	{
		m_nErrorCode = 5;
		m_strLastError = "HTTP server initialization failed";
		return false;
	}

	std::jthread t([this]() { m_pTcpServer->Start(true); });
	m_pHttpServer->Start(true);
	m_pTcpServer->ShutDown();
	return true;
}

void CBootLoader::Finalize()
{
	if (nullptr != m_pHttpServer)
	{
		m_pHttpServer->ShutDown();
	}
	if (nullptr != m_pTcpServer)
	{
		m_pTcpServer->ShutDown();
	}

	m_pHttpServer.reset();
	m_pTcpServer.reset();
	if (nullptr != m_pPython)
	{
		m_pPython->Finalize();
		m_pPython.reset();
	}
	m_bInitialized = false;
}

const std::filesystem::path& CBootLoader::GetRoot() const
{
	return m_root;
}

const std::string& CBootLoader::GetToken() const
{
	return m_strToken;
}

CPythonRuntime& CBootLoader::GetPythonRuntime()
{
	return *m_pPython;
}

net::CTcpServer& CBootLoader::GetTcpServer()
{
	return *m_pTcpServer;
}

net::CHttpServer& CBootLoader::GetHttpServer()
{
	return *m_pHttpServer;
}

const std::string& CBootLoader::GetLastError() const
{
	return m_strLastError;
}

int CBootLoader::GetErrorCode() const
{
	return m_nErrorCode;
}
