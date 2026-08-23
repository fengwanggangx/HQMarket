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
		if (nMilliseconds <= 0)
		{
			return "19900101";
		}
		std::int64_t now =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
				.count();
		if (nMilliseconds > now)
		{
			nMilliseconds = now;
		}
		std::time_t value = static_cast<std::time_t>(nMilliseconds / 1000);
		std::tm date{};
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
		std::tm date{};
		std::istringstream input(value ? value : "");
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
		return value ? static_cast<std::int64_t>(std::llround(PyFloat_AsDouble(value) * std::pow(10.0, scale))) : 0;
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
		PyObject* type = module ? PyObject_GetAttrString(module, "AkShareProvider") : nullptr;
		PyObject* instance = type ? PyObject_CallNoArgs(type) : nullptr;
		bool ok = instance != nullptr;
		Py_XDECREF(type);
		Py_XDECREF(module);
		if (ok)
		{
			m_pProvider = instance;
		}
		else
		{
			PyErr_Clear();
		}
		PyObject* values = ok ? PyObject_CallMethod(instance, "instruments", nullptr) : nullptr;
		if (values && PyList_Check(values))
		{
			for (Py_ssize_t i = 0; i < PyList_Size(values); ++i)
			{
				PyObject* row = PyList_GetItem(values, i);
				PyObject* symbolValue = PyDict_Check(row) ? PyDict_GetItemString(row, "symbol") : nullptr;
				if (symbolValue == nullptr)
				{
					continue;
				}
				const char* symbol = PyUnicode_AsUTF8(symbolValue);
				if (symbol == nullptr)
				{
					continue;
				}
				market::CInstrument instrument;
				instrument.m_strSymbol = symbol;
				if (instrument.m_strSymbol.starts_with('6'))
				{
					instrument.m_exchange = market::Exchange::sse;
				}
				else if (instrument.m_strSymbol.starts_with('0') || instrument.m_strSymbol.starts_with('3'))
				{
					instrument.m_exchange = market::Exchange::szse;
				}
				else
				{
					instrument.m_exchange = market::Exchange::bse;
				}
				m_instruments.emplace_back(std::move(instrument));
			}
		}
		if (values == nullptr)
		{
			PyErr_Clear();
			ok = false;
		}
		Py_XDECREF(values);
		PyGILState_Release(gil);
		std::lock_guard<std::mutex> lock(m_mtx_state);
		m_status = {ok, ok ? "ready" : "initialization failed"};
		return ok;
	}
	bool CAkShareProvider::Subscribe(const std::vector<market::CSubscription>&)
	{
		return false;
	}
	bool CAkShareProvider::Unsubscribe(const std::vector<market::CSubscription>&)
	{
		return false;
	}
	std::vector<market::CBar> CAkShareProvider::QueryBars(const market::CInstrument& instrument, market::Channel channel,
														  std::int64_t nBeginTime, std::int64_t nEndTime)
	{
		std::vector<market::CBar> bars;
		std::unique_lock<std::mutex> providerLock(m_mtx_state);
		if ((channel != market::Channel::bar_1d) || (m_pProvider == nullptr))
		{
			return bars;
		}
		std::string begin = DateString(nBeginTime);
		std::string end = DateString(nEndTime);
		PyGILState_STATE gil = PyGILState_Ensure();
		PyObject* result = PyObject_CallMethod(static_cast<PyObject*>(m_pProvider), "daily_bars", "ssss",
											   instrument.m_strSymbol.c_str(), begin.c_str(), end.c_str(), "");
		bool ok = result && PyList_Check(result);
		if (ok)
		{
			for (Py_ssize_t i = 0; i < PyList_Size(result); ++i)
			{
				PyObject* row = PyList_GetItem(result, i);
				if (!PyDict_Check(row))
				{
					continue;
				}
				market::CBar bar;
				bar.m_instrument = instrument;
				bar.m_channel = channel;
				PyObject* date = PyDict_GetItemString(row, "date");
				bar.m_nBeginTime = DateMilliseconds(date ? PyUnicode_AsUTF8(date) : nullptr);
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
		if (!ok)
		{
			PyErr_Clear();
		}
		Py_XDECREF(result);
		PyGILState_Release(gil);
		m_status = {ok, ok ? "ready" : "history request failed"};
		return bars;
	}
	market::CProviderStatus CAkShareProvider::GetStatus() const
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		return m_status;
	}
	void CAkShareProvider::SetQuoteHandler(_TyQuoteHandler)
	{
	}
	void CAkShareProvider::SetDepthHandler(_TyDepthHandler)
	{
	}
	std::vector<market::CInstrument> CAkShareProvider::QueryInstruments() const
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		return m_instruments;
	}
	void CAkShareProvider::Stop()
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		if ((m_pProvider == nullptr) || !Py_IsInitialized())
		{
			return;
		}
		PyGILState_STATE gil = PyGILState_Ensure();
		Py_DECREF(static_cast<PyObject*>(m_pProvider));
		m_pProvider = nullptr;
		PyGILState_Release(gil);
		m_status = {false, "stopped"};
	}
} // namespace provider
