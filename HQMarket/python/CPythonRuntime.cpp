#include "CPythonRuntime.h"
#include <Python.h>

CPythonRuntime::~CPythonRuntime()
{
	Finalize();
}

bool CPythonRuntime::Initialize(const std::filesystem::path& runtimeHome, const std::filesystem::path& scriptPath)
{
	std::lock_guard<std::mutex> lock(m_mtx_runtime);
	if (m_bInitialized)
	{
		return true;
	}
	PyStatus status;
	PyPreConfig preconfig;
	PyPreConfig_InitIsolatedConfig(&preconfig);
	preconfig.utf8_mode = 1;
	status = Py_PreInitialize(&preconfig);
	if (PyStatus_Exception(status))
	{
		m_strLastError = status.err_msg ? status.err_msg : "Python pre-initialization failed";
		return false;
	}
	PyConfig config;
	PyConfig_InitIsolatedConfig(&config);
	config.use_environment = 0;
	status = PyConfig_SetString(&config, &config.home, runtimeHome.wstring().c_str());
	if (!PyStatus_Exception(status))
	{
		status = PyWideStringList_Append(&config.module_search_paths, scriptPath.wstring().c_str());
	}
	if (!PyStatus_Exception(status))
	{
		status = PyWideStringList_Append(&config.module_search_paths,
			(runtimeHome / "lib" / "python3.12").wstring().c_str());
	}
	if (!PyStatus_Exception(status))
	{
		status = PyWideStringList_Append(&config.module_search_paths,
			(runtimeHome / "lib" / "python3.12" / "site-packages").wstring().c_str());
	}
	if (!PyStatus_Exception(status))
	{
		config.module_search_paths_set = 1;
		status = Py_InitializeFromConfig(&config);
	}
	PyConfig_Clear(&config);
	if (PyStatus_Exception(status))
	{
		m_strLastError = status.err_msg ? status.err_msg : "Python initialization failed";
		return false;
	}
	m_pMainThreadState = PyEval_SaveThread();
	m_bInitialized = true;
	return true;
}

void CPythonRuntime::Finalize()
{
	std::lock_guard<std::mutex> lock(m_mtx_runtime);
	if (!m_bInitialized)
	{
		return;
	}
	PyEval_RestoreThread(m_pMainThreadState);
	Py_FinalizeEx();
	m_pMainThreadState = nullptr;
	m_bInitialized = false;
}

bool CPythonRuntime::IsInitialized() const
{
	std::lock_guard<std::mutex> lock(m_mtx_runtime);
	return m_bInitialized;
}

std::string CPythonRuntime::GetLastError() const
{
	std::lock_guard<std::mutex> lock(m_mtx_runtime);
	return m_strLastError;
}
