#include "CMooTdxProvider.h"
#include <Python.h>
#include <chrono>
#include <cmath>
#include <iostream>

namespace provider
{
	static std::int64_t NowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
			.count();
	}
	static const char* ExchangeName(market::Exchange value)
	{
		if (market::Exchange::sse == value)
		{
			return "SSE";
		}
		if (market::Exchange::szse == value)
		{
			return "SZSE";
		}
		if (market::Exchange::bse == value)
		{
			return "BSE";
		}
		return "UNKNOWN";
	}
	static std::int64_t Fixed(PyObject* dictionary, const char* name, int scale = 4)
	{
		PyObject* value = PyDict_GetItemString(dictionary, name);
		if (nullptr == value)
		{
			return 0;
		}
		return static_cast<std::int64_t>(std::llround(PyFloat_AsDouble(value) * std::pow(10.0, scale)));
	}
	static std::int64_t Integer(PyObject* dictionary, const char* name)
	{
		PyObject* value = PyDict_GetItemString(dictionary, name);
		if (nullptr == value)
		{
			return 0;
		}
		return 0 != PyLong_Check(value) ? PyLong_AsLongLong(value) : static_cast<std::int64_t>(PyFloat_AsDouble(value));
	}
	static void PrintPythonError(const char* context)
	{
		PyObject* errorType = nullptr;
		PyObject* errorValue = nullptr;
		PyObject* errorTraceback = nullptr;
		PyErr_Fetch(&errorType, &errorValue, &errorTraceback);
		PyErr_NormalizeException(&errorType, &errorValue, &errorTraceback);

		PyObject* errorText = nullptr != errorValue ? PyObject_Str(errorValue) : nullptr;
		const char* message = nullptr != errorText ? PyUnicode_AsUTF8(errorText) : nullptr;
		std::cerr << context << ": " << (nullptr != message ? message : "unknown Python error") << std::endl;

		Py_XDECREF(errorText);
		Py_XDECREF(errorTraceback);
		Py_XDECREF(errorValue);
		Py_XDECREF(errorType);
		PyErr_Clear();
	}

	CMooTdxProvider::~CMooTdxProvider()
	{
		Stop();
	}
	const char* CMooTdxProvider::Name() const
	{
		return "mootdx";
	}
	std::string CMooTdxProvider::MakeKey(const market::CChannelInfo& s)
	{
		return s.m_security.String() + ":" + std::to_string(static_cast<int>(s.m_channel));
	}
	bool CMooTdxProvider::Initialize()
	{
		PyGILState_STATE gil = PyGILState_Ensure();
		PyObject* module = PyImport_ImportModule("providers.mootdx_provider");
		PyObject* type = nullptr != module ? PyObject_GetAttrString(module, "MooTdxProvider") : nullptr;
		PyObject* instance = nullptr != type ? PyObject_CallNoArgs(type) : nullptr;
		PyObject* result = nullptr != instance ? PyObject_CallMethod(instance, "initialize", nullptr) : nullptr;
		bool bOk = nullptr != result;
		Py_XDECREF(result);
		Py_XDECREF(type);
		Py_XDECREF(module);
		if (!bOk)
		{
			PrintPythonError("MooTdxProvider initialization failed");
		}
		else
		{
			m_pProvider = instance;
		}
		if (!bOk)
		{
			Py_XDECREF(instance);
		}
		PyGILState_Release(gil);
		{
			std::lock_guard<std::mutex> lck(m_mtx_state);
			m_status = { bOk, bOk ? "connected" : "initialization failed" };
		}
		if (bOk)
		{
			m_bRunning = true;
			m_thread = std::thread(&CMooTdxProvider::Run, this);
		}
		return bOk;
	}
	bool CMooTdxProvider::Subscribe(const std::vector<market::CChannelInfo>& values)
	{
		std::lock_guard<std::mutex> lck(m_mtx_state);
		for (const auto& value : values)
		{
			if ((market::Channel::quote == value.m_channel) || (market::Channel::depth == value.m_channel))
			{
				m_subscriptions.insert_or_assign(MakeKey(value), value);
			}
		}
		return true;
	}
	bool CMooTdxProvider::Unsubscribe(const std::vector<market::CChannelInfo>& values)
	{
		std::lock_guard<std::mutex> lck(m_mtx_state);
		for (const auto& value : values)
		{
			m_subscriptions.erase(MakeKey(value));
		}
		return true;
	}
	std::vector<market::CBar> CMooTdxProvider::QueryBars(const market::CSecurity&, market::Channel, std::int64_t,
														 std::int64_t)
	{
		return {};
	}
	market::CProviderStatus CMooTdxProvider::GetStatus() const
	{
		std::lock_guard<std::mutex> lck(m_mtx_state);
		return m_status;
	}
	void CMooTdxProvider::SetQuoteHandler(_TyQuoteHandler handler)
	{
		std::lock_guard<std::mutex> lck(m_mtx_state);
		m_quoteHandler = std::move(handler);
	}
	void CMooTdxProvider::SetDepthHandler(_TyDepthHandler handler)
	{
		std::lock_guard<std::mutex> lck(m_mtx_state);
		m_depthHandler = std::move(handler);
	}
	void CMooTdxProvider::Run()
	{
		int nFailures = 0;
		int waits[] = { 1, 2, 5, 10, 30 };
		while (m_bRunning)
		{
			std::vector<market::CSecurity> securities;
			{
				std::lock_guard<std::mutex> lck(m_mtx_state);
				std::unordered_map<std::string, market::CSecurity> unique;
				for (const auto& [key, value] : m_subscriptions)
				{
					if ((market::Channel::quote == value.m_channel) || (market::Channel::depth == value.m_channel))
					{
						unique.insert_or_assign(value.m_security.String(), value.m_security);
					}
				}
				for (const auto& [key, security] : unique)
				{
					securities.push_back(security);
				}
			}
			if (securities.empty())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}
			if (Poll(securities))
			{
				nFailures = 0;
				std::this_thread::sleep_for(std::chrono::milliseconds(800));
			}
			else
			{
				int index = nFailures < 4 ? nFailures : 4;
				++nFailures;
				std::this_thread::sleep_for(std::chrono::seconds(waits[index]));
			}
		}
	}
	bool CMooTdxProvider::Poll(const std::vector<market::CSecurity>& securities)
	{
		PyGILState_STATE gil = PyGILState_Ensure();
		PyObject* list = PyList_New(static_cast<Py_ssize_t>(securities.size()));
		for (std::size_t i = 0; i < securities.size(); ++i)
		{
			PyObject* item = Py_BuildValue("{s:s,s:s}", "symbol", securities[i].m_strCode.c_str(), "exchange",
										   ExchangeName(securities[i].m_market));
			PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), item);
		}
		PyObject* result = PyObject_CallMethod(static_cast<PyObject*>(m_pProvider), "quotes", "O", list);
		Py_DECREF(list);
		bool bOk = (nullptr != result) && (0 != PyList_Check(result));
		if (bOk)
		{
			for (Py_ssize_t i = 0; i < PyList_Size(result); ++i)
			{
				PyObject* row = PyList_GetItem(result, i);
				if ((0 == PyDict_Check(row)) || (securities.size() <= static_cast<std::size_t>(i)))
				{
					continue;
				}
				std::int64_t now = NowMs();
				market::CQuote quote;
				quote.m_security = securities[static_cast<std::size_t>(i)];
				quote.m_nReceiveTime = now;
				quote.m_nExchangeTime = now;
				quote.m_nLastPrice = Fixed(row, "price");
				quote.m_nOpenPrice = Fixed(row, "open");
				quote.m_nHighPrice = Fixed(row, "high");
				quote.m_nLowPrice = Fixed(row, "low");
				quote.m_nPreClose = Fixed(row, "last_close");
				quote.m_nVolume = Integer(row, "vol");
				quote.m_nTurnover = Fixed(row, "amount", 2);
				quote.m_strSource = "mootdx";
				market::CDepth depth;
				depth.m_security = quote.m_security;
				depth.m_nReceiveTime = now;
				depth.m_nExchangeTime = now;
				depth.m_strSource = "mootdx";
				for (int level = 1; level <= 5; ++level)
				{
					std::string suffix = std::to_string(level);
					depth.m_bids.push_back(
						{ Fixed(row, ("bid" + suffix).c_str()), Integer(row, ("bid_vol" + suffix).c_str()), 4 });
					depth.m_asks.push_back(
						{ Fixed(row, ("ask" + suffix).c_str()), Integer(row, ("ask_vol" + suffix).c_str()), 4 });
				}
				_TyQuoteHandler quoteHandler;
				_TyDepthHandler depthHandler;
				{
					std::lock_guard<std::mutex> lck(m_mtx_state);
					quoteHandler = m_quoteHandler;
					depthHandler = m_depthHandler;
				}
				if (nullptr != quoteHandler)
				{
					quoteHandler(std::move(quote));
				}
				if (nullptr != depthHandler)
				{
					depthHandler(std::move(depth));
				}
			}
		}
		if (!bOk)
		{
			PyErr_Clear();
		}
		Py_XDECREF(result);
		PyGILState_Release(gil);
		{
			std::lock_guard<std::mutex> lck(m_mtx_state);
			m_status = { bOk, bOk ? "connected" : "quote request failed" };
		}
		return bOk;
	}
	void CMooTdxProvider::Stop()
	{
		if (!m_bRunning.exchange(false))
		{
			return;
		}
		if (m_thread.joinable())
		{
			m_thread.join();
		}
		PyGILState_STATE gil = PyGILState_Ensure();
		Py_XDECREF(static_cast<PyObject*>(m_pProvider));
		m_pProvider = nullptr;
		PyGILState_Release(gil);
	}
} // namespace provider
