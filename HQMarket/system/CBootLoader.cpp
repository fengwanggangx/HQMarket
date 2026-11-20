#include "CBootLoader.h"
#include "CHQCenter.h"
#include "CHQService.h"
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
	if (m_initialized)
	{
		return true;
	}

	m_exec = std::filesystem::current_path();

	m_error_code = 0;
	m_last_error.clear();
	net::EnvInitialize();
	if (!net::IsThreadEnable())
	{
		m_error_code = 1;
		m_last_error = "Failed to enable libevent thread support";
		return false;
	}

	std::string strPyRunTime = ini::CINIHandler::InstanceRef().GetString(ini::Config::System, "System", "py_runtime");
	if (strPyRunTime.empty())
	{
		m_error_code = 2;
		m_last_error = "HQMarket py_runtime is required in ini/system.ini";
		return false;
	}

	std::string strPyScripts = ini::CINIHandler::InstanceRef().GetString(ini::Config::System, "System", "py_scripts");
	if (strPyScripts.empty())
	{
		m_error_code = 3;
		m_last_error = "HQMarket py_scripts is required in ini/system.ini";
		return false;
	}

	m_python = std::make_unique<CPythonRuntime>();
	std::filesystem::path run = m_exec / strPyRunTime;
	std::filesystem::path script = m_exec / strPyScripts;
	if (!m_python->Initialize(run, script))
	{
		m_error_code = 4;
		m_last_error = "Python initialization failed: " + m_python->GetLastError();
		m_python.reset();
		return false;
	}

	ini::CINIHandler& hIni = ini::CINIHandler::InstanceRef();

	static const std::string cs_mysql = "MySQL";
	std::string strMySqlHost = hIni.GetString(ini::Config::System, cs_mysql, "host");
	int nMySqlPort = hIni.GetInt(ini::Config::System, cs_mysql, "port");
	std::string strMySqlAccount = hIni.GetString(ini::Config::System, cs_mysql, "user");
	std::string strMySqlPassword = hIni.GetString(ini::Config::System, cs_mysql, "password");
	std::string strMySqlDatabase = hIni.GetString(ini::Config::System, cs_mysql, "database");
	int nMySqlPoolSize = hIni.GetInt(ini::Config::System, cs_mysql, "pool_size");
	if (strMySqlHost.empty() || (0 >= nMySqlPort) || strMySqlAccount.empty() || strMySqlPassword.empty() || strMySqlDatabase.empty() || (0 >= nMySqlPoolSize))
	{
		m_error_code = 5;
		m_last_error = "HQMarket mysql param error";
		return false;
	}

	db::CConnectParam dbParam(strMySqlHost, static_cast<unsigned int>(nMySqlPort), strMySqlAccount, strMySqlPassword, strMySqlDatabase, "utf8mb4");
	if (0 != CDBEngine::InstanceRef().Initialize(db::em_database::mysql, dbParam, nMySqlPoolSize))
	{
		m_error_code = 6;
		m_last_error = "MySQL initialization failed";
		return false;
	}
	db::_TyDBPtr db = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if ((nullptr == db) || (0 != db->ExecSqlFile(m_exec / "sql" / "table_create.sql")))
	{
		m_error_code = 7;
		m_last_error = "Failed to initialize MySQL tables";
		return false;
	}
	if (0 != db->ExecSqlFile(m_exec / "sql" / "table_init.sql"))
	{
		m_error_code = 8;
		m_last_error = "Failed to execute sql/table_init.sql";
		return false;
	}

	int nTcpPort = hIni.GetInt(ini::Config::System, "System", "tcp_port");
	int nHttpPort = hIni.GetInt(ini::Config::System, "System", "http_port");

	if ((nTcpPort <= 0) || (nHttpPort <= 0))
	{
		m_error_code = 9;
		m_last_error = "Tcp/Http port initialization failed";
		return false;
	}
	m_tcp_server = std::make_unique<net::CTcpServer>(nTcpPort);
	m_http_server = std::make_unique<net::CHttpServer>(nHttpPort);

	m_hq_service = std::make_unique<CHQService>(m_python.get());
	m_hq_center = std::make_unique<CHQCenter>(m_hq_service.get());
	if (!m_hq_center->Initialize() || !m_hq_service->Initialize(m_exec))
	{
		m_error_code = 10;
		m_last_error = "HQMarket service initialization failed";
		return false;
	}
	m_tcp_server->RegisterHandler(std::bind_front(&CHQCenter::OnClientNetEvent, m_hq_center.get()));
	m_http_server->RegisterHandler(net::HttpMethod::GET, "/health", std::bind_front(&CHQCenter::OnHealthRequest, m_hq_center.get()));
	m_http_server->RegisterHandler(net::HttpMethod::GET, "/metrics", std::bind_front(&CHQCenter::OnMetricsRequest, m_hq_center.get()));
	m_http_server->RegisterHandler(net::HttpMethod::GET, "/v1/instruments", std::bind_front(&CHQCenter::OnInstrumentsRequest, m_hq_center.get()));
	m_http_server->RegisterHandler(net::HttpMethod::GET, "/v1/quotes", std::bind_front(&CHQCenter::OnQuoteRequest, m_hq_center.get()));
	m_http_server->RegisterHandler(net::HttpMethod::GET, "/v1/bars", std::bind_front(&CHQCenter::OnBarsRequest, m_hq_center.get()));
	m_initialized = true;
	return true;
}

bool CBootLoader::Run()
{
	if (!m_initialized || (nullptr == m_tcp_server) || (nullptr == m_http_server))
	{
		m_error_code = 11;
		m_last_error = "Boot loader is not initialized";
		return false;
	}
	if (0 != m_tcp_server->Initialize())
	{
		m_error_code = 12;
		m_last_error = "TCP server initialization failed";
		return false;
	}
	if (0 != m_http_server->Initialize())
	{
		m_error_code = 13;
		m_last_error = "HTTP server initialization failed";
		return false;
	}

	std::jthread t([this]()
	{
		m_tcp_server->Start(true);
	});
	m_http_server->Start(true);
	m_tcp_server->ShutDown();
	return true;
}

void CBootLoader::Finalize()
{
	if (nullptr != m_http_server)
	{
		m_http_server->ShutDown();
	}
	if (nullptr != m_tcp_server)
	{
		m_tcp_server->ShutDown();
		m_tcp_server->Drain();
	}
	if (nullptr != m_http_server)
	{
		m_http_server->ClearHandlers();
	}
	if (nullptr != m_hq_service)
	{
		m_hq_service->Stop();
	}
	if (nullptr != m_hq_center)
	{
		m_hq_center->Stop();
	}
	m_hq_center.reset();
	m_hq_service.reset();
	m_http_server.reset();
	m_tcp_server.reset();
	CDBEngine::InstanceRef().Close();
	if (nullptr != m_python)
	{
		m_python->Finalize();
		m_python.reset();
	}
	m_initialized = false;
}

const std::filesystem::path& CBootLoader::GetRoot() const
{
	return m_exec;
}

CPythonRuntime& CBootLoader::GetPythonRuntime()
{
	return *m_python;
}

net::CTcpServer& CBootLoader::GetTcpServer()
{
	return *m_tcp_server;
}

net::CHttpServer& CBootLoader::GetHttpServer()
{
	return *m_http_server;
}

const std::string& CBootLoader::GetLastError() const
{
	return m_last_error;
}

int CBootLoader::GetErrorCode() const
{
	return m_error_code;
}
