#include "CPythonRuntime.h"

#include <Python.h>

#include <mutex>

namespace
{
	std::mutex pythonRuntimeMutex;
	bool bPythonRuntimeOwned{ false };
} // namespace

CPythonRuntime::~CPythonRuntime()
{
	Finalize();
}

bool CPythonRuntime::Initialize(const std::filesystem::path& runtimeHome, const std::filesystem::path& scriptPath)
{
	std::lock_guard<std::mutex> lck(pythonRuntimeMutex);
	if (m_bInitialized)
	{
		return true;
	}
	if (bPythonRuntimeOwned)
	{
		m_strLastError = "Python runtime is already owned by another CPythonRuntime instance";
		return false;
	}
	m_strLastError.clear();

	PyStatus status;
	PyPreConfig preConfig;
	PyPreConfig_InitIsolatedConfig(&preConfig);
	preConfig.utf8_mode = 1;
	status = Py_PreInitialize(&preConfig);
	if (0 != PyStatus_Exception(status))
	{
		m_strLastError = (nullptr != status.err_msg) ? status.err_msg : "Python pre-initialization failed";
		return false;
	}

	PyConfig config;
	PyConfig_InitIsolatedConfig(&config);
	config.use_environment = 0;
	status = PyWideStringList_Append(&config.module_search_paths, scriptPath.wstring().c_str());
	if (0 == PyStatus_Exception(status))
	{
		status = PyWideStringList_Append(&config.module_search_paths, runtimeHome.wstring().c_str());
	}
	if (0 == PyStatus_Exception(status))
	{
		status = PyWideStringList_Append(&config.module_search_paths, (runtimeHome / "lib-dynload").wstring().c_str());
	}
	if (0 == PyStatus_Exception(status))
	{
		status = PyWideStringList_Append(&config.module_search_paths, (runtimeHome / "site-packages").wstring().c_str());
	}
	if (0 == PyStatus_Exception(status))
	{
		config.module_search_paths_set = 1;
		status = Py_InitializeFromConfig(&config);
	}
	if (0 != PyStatus_Exception(status))
	{
		m_strLastError = (nullptr != status.err_msg) ? status.err_msg : "Python initialization failed";
		PyConfig_Clear(&config);
		return false;
	}
	PyConfig_Clear(&config);

	m_pMainThreadState = PyEval_SaveThread();
	m_bInitialized = true;
	bPythonRuntimeOwned = true;
	return true;
}

void CPythonRuntime::Finalize()
{
	std::lock_guard<std::mutex> lck(pythonRuntimeMutex);
	if (!m_bInitialized)
	{
		return;
	}
	PyEval_RestoreThread(m_pMainThreadState);
	int finalizeResult = Py_FinalizeEx();
	m_pMainThreadState = nullptr;
	m_bInitialized = false;
	bPythonRuntimeOwned = false;
	if (0 != finalizeResult)
	{
		m_strLastError = "Python finalization failed";
	}
}

bool CPythonRuntime::IsInitialized() const
{
	std::lock_guard<std::mutex> lck(pythonRuntimeMutex);
	return m_bInitialized;
}

std::string CPythonRuntime::GetLastError() const
{
	std::lock_guard<std::mutex> lck(pythonRuntimeMutex);
	return m_strLastError;
}
