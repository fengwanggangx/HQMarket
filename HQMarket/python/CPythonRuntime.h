#ifndef HQMARKET_PYTHON_CPYTHONRUNTIME_H
#define HQMARKET_PYTHON_CPYTHONRUNTIME_H

#include <filesystem>
#include <string>

struct _ts;

class CPythonRuntime final
{
	public:
	CPythonRuntime() = default;
	~CPythonRuntime();
	bool Initialize(const std::filesystem::path& runtimeHome, const std::filesystem::path& scriptPath);
	void Finalize();
	bool IsInitialized() const;
	std::string GetLastError() const;

	private:
	_ts* m_pMainThreadState{ nullptr };
	bool m_bInitialized{ false };
	std::string m_strLastError;
};

#endif
