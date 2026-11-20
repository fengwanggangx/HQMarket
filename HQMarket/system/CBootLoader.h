#ifndef HQMARKET_SYSTEM_CBOOTLOADER_H
#define HQMARKET_SYSTEM_CBOOTLOADER_H

#include <filesystem>
#include <memory>
#include <string>
#include <thread>

class CPythonRuntime;
class CHQService;
class CHQCenter;

namespace net
{
	class CHttpServer;
	class CTcpServer;
} // namespace net

class CBootLoader final
{
public:
	CBootLoader();
	~CBootLoader();
	CBootLoader(const CBootLoader& arg) = delete;
	CBootLoader& operator=(const CBootLoader& arg) = delete;

	bool Initialize();
	bool Run();
	void Finalize();
	const std::filesystem::path& GetRoot() const;
	CPythonRuntime& GetPythonRuntime();
	net::CTcpServer& GetTcpServer();
	net::CHttpServer& GetHttpServer();
	const std::string& GetLastError() const;
	int GetErrorCode() const;

private:
	std::filesystem::path m_exec;
	std::filesystem::path m_path_py_runtime;
	std::filesystem::path m_path_py_scripts;
	std::string m_last_error;
	int m_error_code{ 0 };
	bool m_initialized{ false };

private:
	std::unique_ptr<CHQService> m_hq_service;
	std::unique_ptr<CHQCenter> m_hq_center;
	std::unique_ptr<CPythonRuntime> m_python;
	std::unique_ptr<net::CTcpServer> m_tcp_server;
	std::unique_ptr<net::CHttpServer> m_http_server;
};

#endif
