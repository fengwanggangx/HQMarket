#ifndef HQMARKET_SYSTEM_CBOOTLOADER_H
#define HQMARKET_SYSTEM_CBOOTLOADER_H

#include <filesystem>
#include <memory>
#include <string>
#include <thread>

class CPythonRuntime;

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
		CBootLoader(const CBootLoader&) = delete;
		CBootLoader& operator=(const CBootLoader&) = delete;

		bool Initialize();
		bool Run();
		void Finalize();
		const std::filesystem::path& GetRoot() const;
		const std::string& GetToken() const;
		CPythonRuntime& GetPythonRuntime();
		net::CTcpServer& GetTcpServer();
		net::CHttpServer& GetHttpServer();
		const std::string& GetLastError() const;
		int GetErrorCode() const;

	private:
		std::filesystem::path m_root;
		std::string m_strToken;
		std::string m_strLastError;
		int m_nErrorCode{ 0 };
		bool m_bInitialized{ false };

	private:
		std::unique_ptr<CPythonRuntime> m_pPython;
		std::unique_ptr<net::CTcpServer> m_pTcpServer;
		std::unique_ptr<net::CHttpServer> m_pHttpServer;
};

#endif
