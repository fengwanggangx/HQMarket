#include "CMooTdxProvider.h"
#include <Python.h>
#include <chrono>
#include <cmath>

namespace provider
{
	static std::int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
	static const char* ExchangeName(market::Exchange value) { if (value == market::Exchange::sse) return "SSE"; if (value == market::Exchange::szse) return "SZSE"; if (value == market::Exchange::bse) return "BSE"; return "UNKNOWN"; }
	static std::int64_t Fixed(PyObject* dictionary, const char* name, int scale = 4) { PyObject* value = PyDict_GetItemString(dictionary, name); if (!value) return 0; return static_cast<std::int64_t>(std::llround(PyFloat_AsDouble(value) * std::pow(10.0, scale))); }
	static std::int64_t Integer(PyObject* dictionary, const char* name) { PyObject* value = PyDict_GetItemString(dictionary, name); if (!value) return 0; return PyLong_Check(value) ? PyLong_AsLongLong(value) : static_cast<std::int64_t>(PyFloat_AsDouble(value)); }

	CMooTdxProvider::~CMooTdxProvider() { Stop(); }
	const char* CMooTdxProvider::Name() const { return "mootdx"; }
	std::string CMooTdxProvider::MakeKey(const market::CSubscription& s) { return s.m_instrument.Key() + ":" + std::to_string(static_cast<int>(s.m_channel)); }
	bool CMooTdxProvider::Initialize()
	{
		PyGILState_STATE gil = PyGILState_Ensure(); PyObject* module = PyImport_ImportModule("providers.mootdx_provider"); PyObject* type = module ? PyObject_GetAttrString(module, "MooTdxProvider") : nullptr; PyObject* instance = type ? PyObject_CallNoArgs(type) : nullptr; PyObject* result = instance ? PyObject_CallMethod(instance, "initialize", nullptr) : nullptr; const bool ok = result != nullptr; Py_XDECREF(result); Py_XDECREF(type); Py_XDECREF(module);
		if (!ok) PyErr_Clear(); else m_pProvider = instance; if (!ok) Py_XDECREF(instance); PyGILState_Release(gil);
		{ std::lock_guard lock(m_mtxState); m_status = { ok, ok ? "connected" : "initialization failed" }; }
		if (ok) { m_bRunning = true; m_thread = std::thread(&CMooTdxProvider::Run, this); } return ok;
	}
	bool CMooTdxProvider::Subscribe(const std::vector<market::CSubscription>& values) { std::lock_guard lock(m_mtxState); for (const auto& value : values) if ((value.m_channel == market::Channel::quote) || (value.m_channel == market::Channel::depth)) m_subscriptions.insert_or_assign(MakeKey(value), value); return true; }
	bool CMooTdxProvider::Unsubscribe(const std::vector<market::CSubscription>& values) { std::lock_guard lock(m_mtxState); for (const auto& value : values) m_subscriptions.erase(MakeKey(value)); return true; }
	std::vector<market::CBar> CMooTdxProvider::QueryBars(const market::CInstrument&, market::Channel, std::int64_t, std::int64_t) { return {}; }
	market::CProviderStatus CMooTdxProvider::GetStatus() const { std::lock_guard lock(m_mtxState); return m_status; }
	void CMooTdxProvider::SetQuoteHandler(_TyQuoteHandler handler) { std::lock_guard lock(m_mtxState); m_quoteHandler = std::move(handler); }
	void CMooTdxProvider::SetDepthHandler(_TyDepthHandler handler) { std::lock_guard lock(m_mtxState); m_depthHandler = std::move(handler); }
	void CMooTdxProvider::Run()
	{
		int nFailures = 0; const int waits[] = { 1, 2, 5, 10, 30 };
		while (m_bRunning) { std::vector<market::CInstrument> instruments; { std::lock_guard lock(m_mtxState); std::unordered_map<std::string, market::CInstrument> unique; for (const auto& [key, value] : m_subscriptions) if ((value.m_channel == market::Channel::quote) || (value.m_channel == market::Channel::depth)) unique.insert_or_assign(value.m_instrument.Key(), value.m_instrument); for (const auto& [key, instrument] : unique) instruments.push_back(instrument); }
			if (instruments.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
			if (Poll(instruments)) { nFailures = 0; std::this_thread::sleep_for(std::chrono::milliseconds(800)); } else { const int index = nFailures < 4 ? nFailures : 4; ++nFailures; std::this_thread::sleep_for(std::chrono::seconds(waits[index])); }
		}
	}
	bool CMooTdxProvider::Poll(const std::vector<market::CInstrument>& instruments)
	{
		PyGILState_STATE gil = PyGILState_Ensure(); PyObject* list = PyList_New(static_cast<Py_ssize_t>(instruments.size()));
		for (std::size_t i = 0; i < instruments.size(); ++i) { PyObject* item = Py_BuildValue("{s:s,s:s}", "symbol", instruments[i].m_strSymbol.c_str(), "exchange", ExchangeName(instruments[i].m_exchange)); PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), item); }
		PyObject* result = PyObject_CallMethod(static_cast<PyObject*>(m_pProvider), "quotes", "O", list); Py_DECREF(list); const bool ok = result && PyList_Check(result);
		if (ok) for (Py_ssize_t i = 0; i < PyList_Size(result); ++i) { PyObject* row = PyList_GetItem(result, i); if (!PyDict_Check(row) || static_cast<std::size_t>(i) >= instruments.size()) continue; const auto now = NowMs(); market::CQuote quote; quote.m_instrument = instruments[static_cast<std::size_t>(i)]; quote.m_nReceiveTime = now; quote.m_nExchangeTime = now; quote.m_nLastPrice = Fixed(row, "price"); quote.m_nOpenPrice = Fixed(row, "open"); quote.m_nHighPrice = Fixed(row, "high"); quote.m_nLowPrice = Fixed(row, "low"); quote.m_nPreClose = Fixed(row, "last_close"); quote.m_nVolume = Integer(row, "vol"); quote.m_nTurnover = Fixed(row, "amount", 2); quote.m_strSource = "mootdx"; market::CDepth depth; depth.m_instrument = quote.m_instrument; depth.m_nReceiveTime = now; depth.m_nExchangeTime = now; depth.m_strSource = "mootdx"; for (int level = 1; level <= 5; ++level) { const auto suffix = std::to_string(level); depth.m_bids.push_back({ Fixed(row, ("bid" + suffix).c_str()), Integer(row, ("bid_vol" + suffix).c_str()), 4 }); depth.m_asks.push_back({ Fixed(row, ("ask" + suffix).c_str()), Integer(row, ("ask_vol" + suffix).c_str()), 4 }); } _TyQuoteHandler quoteHandler; _TyDepthHandler depthHandler; { std::lock_guard lock(m_mtxState); quoteHandler = m_quoteHandler; depthHandler = m_depthHandler; } if (quoteHandler) quoteHandler(std::move(quote)); if (depthHandler) depthHandler(std::move(depth)); }
		if (!ok) PyErr_Clear(); Py_XDECREF(result); PyGILState_Release(gil); { std::lock_guard lock(m_mtxState); m_status = { ok, ok ? "connected" : "quote request failed" }; } return ok;
	}
	void CMooTdxProvider::Stop() { if (!m_bRunning.exchange(false)) return; if (m_thread.joinable()) m_thread.join(); PyGILState_STATE gil = PyGILState_Ensure(); Py_XDECREF(static_cast<PyObject*>(m_pProvider)); m_pProvider = nullptr; PyGILState_Release(gil); }
}
