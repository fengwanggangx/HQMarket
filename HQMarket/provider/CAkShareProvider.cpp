#include "CAkShareProvider.h"
#include <Python.h>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
namespace provider
{
	static std::string DateString(std::int64_t nMilliseconds)
	{
		if (0 >= nMilliseconds)
		{
			return "19900101";
		}
		std::int64_t now =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
				.count();
		if (now < nMilliseconds)
		{
			nMilliseconds = now;
		}
		std::time_t value = static_cast<std::time_t>(nMilliseconds / 1000);
		std::tm date{ };
#ifdef _WIN32
		gmtime_s(&date, &value);
#else
		gmtime_r(&value, &date);
#endif
		std::ostringstream out;
		out << std::put_time(&date, "%Y%m%d");
		return out.str();
	}
	static std::int64_t DateMilliseconds(const char* value)
	{
		std::tm date{ };
		std::istringstream input(nullptr != value ? value : "");
		input >> std::get_time(&date, "%Y-%m-%d");
#ifdef _WIN32
		return static_cast<std::int64_t>(_mkgmtime(&date)) * 1000;
#else
		return static_cast<std::int64_t>(timegm(&date)) * 1000;
#endif
	}
	static std::int64_t Fixed(PyObject* row, const char* name, int scale)
	{
		PyObject* value = PyDict_GetItemString(row, name);
		return nullptr != value ? static_cast<std::int64_t>(std::llround(PyFloat_AsDouble(value) * std::pow(10.0, scale))) : 0;
	}
	CAkShareProvider::~CAkShareProvider()
	{
		Stop();
	}
	const char* CAkShareProvider::Name() const
	{
		return "akshare";
	}
	bool CAkShareProvider::Initialize()
	{
		PyGILState_STATE gil = PyGILState_Ensure();
		PyObject* module = PyImport_ImportModule("providers.akshare_provider");
		PyObject* type = nullptr != module ? PyObject_GetAttrString(module, "AkShareProvider") : nullptr;
		PyObject* instance = nullptr != type ? PyObject_CallNoArgs(type) : nullptr;
		bool bOk = nullptr != instance;
		Py_XDECREF(type);
		Py_XDECREF(module);
		if (bOk)
		{
			m_pProvider = instance;
		}
		else
		{
			PyErr_Clear();
		}
		PyObject* values = bOk ? PyObject_CallMethod(instance, "instruments", nullptr) : nullptr;
		if ((nullptr != values) && (0 != PyList_Check(values)))
		{
			for (Py_ssize_t i = 0; i < PyList_Size(values); ++i)
			{
				PyObject* row = PyList_GetItem(values, i);
				PyObject* symbolValue = PyDict_Check(row) ? PyDict_GetItemString(row, "symbol") : nullptr;
				if (nullptr == symbolValue)
				{
					continue;
				}
				const char* symbol = PyUnicode_AsUTF8(symbolValue);
				if (nullptr == symbol)
				{
					continue;
				}
				market::CSecurity security;
				security.m_strCode = symbol;
				if (security.m_strCode.starts_with('6'))
				{
					security.m_market = market::Exchange::sse;
				}
				else if (security.m_strCode.starts_with('0') || security.m_strCode.starts_with('3'))
				{
					security.m_market = market::Exchange::szse;
				}
				else
				{
					security.m_market = market::Exchange::bse;
				}
				m_instruments.emplace_back(std::move(security));
			}
		}
		if (nullptr == values)
		{
			PyErr_Clear();
			bOk = false;
		}
		Py_XDECREF(values);
		PyGILState_Release(gil);
		std::lock_guard<std::mutex> lck(m_mtx_state);
		m_status = { bOk, bOk ? "ready" : "initialization failed" };
		return bOk;
	}
	bool CAkShareProvider::Subscribe(const std::vector<market::CSubscription>&)
	{
		return false;
	}
	bool CAkShareProvider::Unsubscribe(const std::vector<market::CSubscription>&)
	{
		return false;
	}
	std::vector<market::CBar> CAkShareProvider::QueryBars(const market::CSecurity& security, market::Channel channel,
														  std::int64_t nBeginTime, std::int64_t nEndTime)
	{
		std::vector<market::CBar> bars;
		std::unique_lock<std::mutex> lck(m_mtx_state);
		if ((market::Channel::bar_1d != channel) || (nullptr == m_pProvider))
		{
			return bars;
		}
		std::string begin = DateString(nBeginTime);
		std::string end = DateString(nEndTime);
		PyGILState_STATE gil = PyGILState_Ensure();
		PyObject* result = PyObject_CallMethod(static_cast<PyObject*>(m_pProvider), "daily_bars", "ssss",
											   security.m_strCode.c_str(), begin.c_str(), end.c_str(), "");
		bool bOk = (nullptr != result) && (0 != PyList_Check(result));
		if (bOk)
		{
			for (Py_ssize_t i = 0; i < PyList_Size(result); ++i)
			{
				PyObject* row = PyList_GetItem(result, i);
				if (0 == PyDict_Check(row))
				{
					continue;
				}
				market::CBar bar;
				bar.m_security = security;
				bar.m_channel = channel;
				PyObject* date = PyDict_GetItemString(row, "date");
				bar.m_nBeginTime = DateMilliseconds(nullptr != date ? PyUnicode_AsUTF8(date) : nullptr);
				bar.m_nOpenPrice = Fixed(row, "open", 4);
				bar.m_nHighPrice = Fixed(row, "high", 4);
				bar.m_nLowPrice = Fixed(row, "low", 4);
				bar.m_nClosePrice = Fixed(row, "close", 4);
				bar.m_nVolume = Fixed(row, "volume", 0);
				bar.m_nTurnover = Fixed(row, "turnover", 2);
				bar.m_strSource = "akshare";
				bars.emplace_back(std::move(bar));
			}
		}
		if (!bOk)
		{
			PyErr_Clear();
		}
		Py_XDECREF(result);
		PyGILState_Release(gil);
		m_status = { bOk, bOk ? "ready" : "history request failed" };
		return bars;
	}
	market::CProviderStatus CAkShareProvider::GetStatus() const
	{
		std::lock_guard<std::mutex> lck(m_mtx_state);
		return m_status;
	}
	void CAkShareProvider::SetQuoteHandler(_TyQuoteHandler)
	{
	}
	void CAkShareProvider::SetDepthHandler(_TyDepthHandler)
	{
	}
	std::vector<market::CSecurity> CAkShareProvider::QueryInstruments() const
	{
		std::lock_guard<std::mutex> lck(m_mtx_state);
		return m_instruments;
	}
	void CAkShareProvider::Stop()
	{
		std::lock_guard<std::mutex> lck(m_mtx_state);
		if ((nullptr == m_pProvider) || (0 == Py_IsInitialized()))
		{
			return;
		}
		PyGILState_STATE gil = PyGILState_Ensure();
		Py_DECREF(static_cast<PyObject*>(m_pProvider));
		m_pProvider = nullptr;
		PyGILState_Release(gil);
		m_status = { false, "stopped" };
	}
} // namespace provider
