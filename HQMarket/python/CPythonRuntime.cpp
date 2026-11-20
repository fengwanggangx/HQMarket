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
	if (m_initialized)
	{
		return true;
	}
	if (bPythonRuntimeOwned)
	{
		m_last_error = "Python runtime is already owned by another CPythonRuntime instance";
		return false;
	}
	m_last_error.clear();

	PyStatus status;
	PyPreConfig preConfig;
	PyPreConfig_InitIsolatedConfig(&preConfig);
	preConfig.utf8_mode = 1;
	status = Py_PreInitialize(&preConfig);
	if (0 != PyStatus_Exception(status))
	{
		m_last_error = (nullptr != status.err_msg) ? status.err_msg : "Python pre-initialization failed";
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
		m_last_error = (nullptr != status.err_msg) ? status.err_msg : "Python initialization failed";
		PyConfig_Clear(&config);
		return false;
	}
	PyConfig_Clear(&config);

	m_main_thread_state = PyEval_SaveThread();
	m_initialized = true;
	bPythonRuntimeOwned = true;
	return true;
}

void CPythonRuntime::Finalize()
{
	std::lock_guard<std::mutex> lck(pythonRuntimeMutex);
	if (!m_initialized)
	{
		return;
	}
	PyEval_RestoreThread(m_main_thread_state);
	int finalizeResult = Py_FinalizeEx();
	m_main_thread_state = nullptr;
	m_initialized = false;
	bPythonRuntimeOwned = false;
	if (0 != finalizeResult)
	{
		m_last_error = "Python finalization failed";
	}
}

bool CPythonRuntime::IsInitialized() const
{
	std::lock_guard<std::mutex> lck(pythonRuntimeMutex);
	return m_initialized;
}

std::string CPythonRuntime::GetLastError() const
{
	std::lock_guard<std::mutex> lck(pythonRuntimeMutex);
	return m_last_error;
}
