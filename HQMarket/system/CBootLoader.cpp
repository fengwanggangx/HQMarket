#include "CBootLoader.h"
#include "../database/CDBEngine.h"
#include "../database/IDataBase.h"
#include "../ini/CINIHandler.h"
#include "../network/CHttpServer.h"
#include "../network/CTcpServer.h"
#include "../python/CPythonRuntime.h"

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

	m_exec = std::filesystem::current_path();

	m_nErrorCode = 0;
	m_strLastError.clear();
	net::EnvInitialize();
	if (!net::IsThreadEnable())
	{
		m_nErrorCode = 1;
		m_strLastError = "Failed to enable libevent thread support";
		return false;
	}

	std::string strPyRunTime = ini::CINIHandler::InstanceRef().GetValue(ini::Config::System, "System", "py_runtime", std::string());
	if (strPyRunTime.empty())
	{
		m_nErrorCode = 3;
		m_strLastError = "HQMarket py_runtime is required in ini/system.ini";
		return false;
	}

	std::string strPyScripts = ini::CINIHandler::InstanceRef().GetValue(ini::Config::System, "System", "py_scripts", std::string());
	if (strPyScripts.empty())
	{
		m_nErrorCode = 4;
		m_strLastError = "HQMarket py_scripts is required in ini/system.ini";
		return false;
	}

	m_pPython = std::make_unique<CPythonRuntime>();
	std::filesystem::path run = m_exec / strPyRunTime;
	std::filesystem::path script = m_exec / strPyScripts;
	if (!m_pPython->Initialize(run, script))
	{
		m_nErrorCode = 5;
		m_strLastError = "Python initialization failed: " + m_pPython->GetLastError();
		m_pPython.reset();
		return false;
	}

	ini::CINIHandler& hIni = ini::CINIHandler::InstanceRef();
	std::string strMySqlHost = hIni.GetValue(ini::Config::System, "MySQL", "host", std::string());
	int nMySqlPort = hIni.GetValue(ini::Config::System, "MySQL", "port", -1);
	std::string strMySqlAccount = hIni.GetValue(ini::Config::System, "MySQL", "account", std::string());
	std::string strMySqlPassword = hIni.GetValue(ini::Config::System, "MySQL", "password", std::string());
	std::string strMySqlDatabase = hIni.GetValue(ini::Config::System, "MySQL", "database", std::string());
	int nMySqlPoolSize = hIni.GetValue(ini::Config::System, "MySQL", "pool_size", -1);
	if (strMySqlHost.empty() || (0 >= nMySqlPort) || strMySqlAccount.empty() || strMySqlPassword.empty() || strMySqlDatabase.empty() || (0 >= nMySqlPoolSize))
	{
		m_nErrorCode = 6;
		m_strLastError = "HQMarket mysql param error";
		return false;
	}

	db::CConnectParam dbParam(strMySqlHost, static_cast<unsigned int>(nMySqlPort), strMySqlAccount, strMySqlPassword, strMySqlDatabase, "utf8mb4");
	if (0 != CDBEngine::InstanceRef().Initialize(db::em_database::mysql, dbParam, nMySqlPoolSize))
	{
		m_nErrorCode = 7;
		m_strLastError = "MySQL initialization failed";
		return false;
	}
	db::_TyDBPtr db = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if ((nullptr == db) || (0 != db->ExecSqlFile(m_exec / "sql" / "table_create.sql")))
	{
		m_nErrorCode = 8;
		m_strLastError = "Failed to initialize MySQL tables";
		return false;
	}

	int nTcpPort = hIni.GetValue(ini::Config::System, "System", "tcp_port", -1);
	int nHttpPort = hIni.GetValue(ini::Config::System, "System", "http_port", -1);

	if ((nTcpPort <= 0) || (nHttpPort <= 0))
	{
		m_nErrorCode = 9;
		m_strLastError = "Tcp/Http port initialization failed";
		return false;
	}
	m_pTcpServer = std::make_unique<net::CTcpServer>(nTcpPort);
	m_pHttpServer = std::make_unique<net::CHttpServer>(nHttpPort);

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
	CDBEngine::InstanceRef().Close();
	if (nullptr != m_pPython)
	{
		m_pPython->Finalize();
		m_pPython.reset();
	}
	m_bInitialized = false;
}

const std::filesystem::path& CBootLoader::GetRoot() const
{
	return m_exec;
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
