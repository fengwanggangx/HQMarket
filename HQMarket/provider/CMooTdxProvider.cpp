#include "../log/CLogger.h"
#include "CMooTdxProvider.h"
#include <Python.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <iostream>

namespace provider
{
	static std::int64_t NowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
			.count();
	}

	static bool InMarketSession(std::int64_t now, const std::vector<std::pair<int, int>>& sessions)
	{
		int minute = static_cast<int>((now / 60000 + 480) % 1440);
		return std::ranges::any_of(sessions, [minute](const auto& session)
		{
			return session.first <= minute && minute < session.second;
		});
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
		double scaled = PyFloat_AsDouble(value) * std::pow(10.0, scale);
		if (nullptr != PyErr_Occurred())
		{
			PyErr_Clear();
			return 0;
		}
		double limit = static_cast<double>((std::numeric_limits<std::int64_t>::max)());
		if (!std::isfinite(scaled) || scaled >= limit || scaled < -limit)
		{
			return 0;
		}
		return static_cast<std::int64_t>(std::llround(scaled));
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
	static std::string String(PyObject* dictionary, const char* name)
	{
		PyObject* value = PyDict_GetItemString(dictionary, name);
		const char* pValue = nullptr != value ? PyUnicode_AsUTF8(value) : nullptr;
		if (nullptr == pValue)
		{
			PyErr_Clear();
			return {};
		}
		return pValue;
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
		CLogger::Write(Level::Error, "provider", context, ": ", (nullptr != message ? message : "unknown Python error"));

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
			m_provider = instance;
		}
		if (!bOk)
		{
			Py_XDECREF(instance);
		}
		PyGILState_Release(gil);
		{
			std::lock_guard<std::mutex> lck(m_state_mutex);
			m_status = { bOk, bOk ? "connected" : "initialization failed" };
		}
		if (bOk)
		{
			m_running = true;
			m_thread = std::thread(&CMooTdxProvider::Run, this);
		}
		return bOk;
	}
	bool CMooTdxProvider::Subscribe(const std::vector<market::CChannelInfo>& values)
	{
		std::lock_guard<std::mutex> lck(m_state_mutex);
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
		std::lock_guard<std::mutex> lck(m_state_mutex);
		for (const auto& value : values)
		{
			m_subscriptions.erase(MakeKey(value));
			m_last_refresh.erase(value.m_security.String());
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
		std::lock_guard<std::mutex> lck(m_state_mutex);
		return m_status;
	}
	void CMooTdxProvider::SetQuoteHandler(_TyQuoteHandler handler)
	{
		std::lock_guard<std::mutex> lck(m_state_mutex);
		m_quote_handler = std::move(handler);
	}
	void CMooTdxProvider::SetDepthHandler(_TyDepthHandler handler)
	{
		std::lock_guard<std::mutex> lck(m_state_mutex);
		m_depth_handler = std::move(handler);
	}
	void CMooTdxProvider::Run()
	{
		int nFailures = 0;
		int waits[] = { 1, 2, 5, 10, 30 };
		std::vector<market::CSecurity> scanSecurities;
		std::vector<market::CSecurity> hongKong;
		std::vector<market::CQuote> scanQuotes;
		_TyScanHandler scanHandler;
		std::size_t scanOffset = 0;
		std::int64_t scanStarted = 0;
		std::chrono::steady_clock::time_point nextHongKong;
		bool configured = false;
		std::uint64_t scanRevision = 0;
		std::filesystem::path calendarPath;
		std::int64_t calendarDate = -1;
		std::vector<std::pair<int, int>> mainlandSessions;
		std::vector<std::pair<int, int>> hongKongSessions;
		while (m_running)
		{
			std::vector<market::CSecurity> securities;
			{
				std::lock_guard<std::mutex> lck(m_state_mutex);
				if (0 == scanOffset && scanRevision != m_scan_revision)
				{
					scanSecurities.clear();
					hongKong.clear();
					for (const auto& security : m_scan_securities)
					{
						(market::Exchange::hkex == security.m_market ? hongKong : scanSecurities).push_back(security);
					}
					scanHandler = m_scan_handler;
					calendarPath = m_scan_calendar;
					configured = true;
					scanRevision = m_scan_revision;
				}
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
			std::int64_t now = NowMs();
			std::int64_t day = market::MarketDateIndex(now);
			if (configured && calendarDate != day)
			{
				calendarDate = day;
				mainlandSessions.clear();
				hongKongSessions.clear();
				std::chrono::sys_days date{ std::chrono::days{ day } };
				std::chrono::year_month_day localDate{ date };
				int dateNumber = static_cast<int>(localDate.year()) * 10000 + static_cast<int>(static_cast<unsigned>(localDate.month())) * 100 + static_cast<int>(static_cast<unsigned>(localDate.day()));
				std::ifstream calendar(calendarPath);
				if (!calendar.is_open() && std::chrono::weekday{ date }.iso_encoding() <= 5)
				{
					mainlandSessions = { { 555, 690 }, { 780, 905 } };
					hongKongSessions = { { 540, 720 }, { 780, 970 } };
				}
				int sessionDate = 0;
				int begin = 0;
				int end = 0;
				std::string market;
				while (calendar >> sessionDate >> market >> begin >> end)
				{
					if (dateNumber == sessionDate && 0 <= begin && begin < end && end <= 1440 && ("CN" == market || "HK" == market))
					{
						("HK" == market ? hongKongSessions : mainlandSessions).emplace_back(begin, end);
					}
				}
			}
			bool scanHongKong = !hongKong.empty() && InMarketSession(now, hongKongSessions) && nextHongKong <= std::chrono::steady_clock::now();
			bool scanMainland = scanOffset < scanSecurities.size() && InMarketSession(now, mainlandSessions);
			if (!scanMainland && 0 != scanOffset)
			{
				scanOffset = 0;
				scanQuotes.clear();
			}
			if (scanHongKong || scanMainland)
			{
				if (0 != scanOffset && market::MarketDateIndex(scanStarted) != market::MarketDateIndex(NowMs()))
				{
					scanOffset = 0;
					scanQuotes.clear();
				}
				// Run at most one upstream scan batch between subscription refreshes.
				if (!securities.empty())
				{
					std::size_t count = Poll(securities);
					std::lock_guard<std::mutex> lock(m_state_mutex);
					bool complete = count == securities.size();
					m_status = { complete, complete ? "connected" : "missing, stale or unavailable quotes" };
				}
				if (scanHongKong)
				{
					std::int64_t started = NowMs();
					std::vector<market::CQuote> quotes;
					Poll(hongKong, &quotes);
					scanHandler(true, hongKong.size(), started, NowMs(), std::move(quotes));
					nextHongKong = std::chrono::steady_clock::now() + std::chrono::seconds(120);
				}
				else
				{
					if (0 == scanOffset)
					{
						scanStarted = NowMs();
						scanQuotes.reserve(scanSecurities.size());
					}
					std::size_t end = (std::min)(scanOffset + 80, scanSecurities.size());
					std::vector<market::CSecurity> batch(scanSecurities.begin() + scanOffset, scanSecurities.begin() + end);
					Poll(batch, &scanQuotes);
					scanOffset = end;
					if (scanOffset == scanSecurities.size())
					{
						scanHandler(false, scanSecurities.size(), scanStarted, NowMs(), std::move(scanQuotes));
						scanQuotes.clear();
						scanOffset = 0;
					}
				}
				Wait(std::chrono::milliseconds(800));
				continue;
			}
			if (securities.empty())
			{
				Wait(std::chrono::milliseconds(200));
				continue;
			}
			constexpr std::size_t BatchSize = 1000;
			std::size_t nSize = securities.size();
			std::size_t validCount = 0;
			for (std::size_t nOffset = 0; nSize > nOffset; nOffset += BatchSize)
			{
				std::size_t nEnd = (std::min)(nSize, nOffset + BatchSize);
				std::vector<market::CSecurity> batch(securities.begin() + static_cast<std::ptrdiff_t>(nOffset),
					securities.begin() + static_cast<std::ptrdiff_t>(nEnd));
				validCount += Poll(batch);
			}
			{
				std::lock_guard<std::mutex> lck(m_state_mutex);
				bool bComplete = validCount == nSize;
				m_status = { bComplete, bComplete ? "connected" : "missing, stale or unavailable quotes" };
			}
			if (0 != validCount)
			{
				nFailures = 0;
				Wait(std::chrono::milliseconds(800));
			}
			else
			{
				int index = nFailures < 4 ? nFailures : 4;
				++nFailures;
				Wait(std::chrono::seconds(waits[index]));
			}
		}
	}
	std::size_t CMooTdxProvider::Poll(const std::vector<market::CSecurity>& securities, std::vector<market::CQuote>* scan)
	{
		std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		PyGILState_STATE gil = PyGILState_Ensure();
		std::unordered_map<std::string, market::CSecurity> securityByKey;
		securityByKey.reserve(securities.size());
		PyObject* list = PyList_New(static_cast<Py_ssize_t>(securities.size()));
		for (std::size_t i = 0; i < securities.size(); ++i)
		{
			securityByKey.emplace(securities[i].m_code + ":" + ExchangeName(securities[i].m_market), securities[i]);
			PyObject* item = Py_BuildValue("{s:s,s:s}", "symbol", securities[i].m_code.c_str(), "exchange",
				ExchangeName(securities[i].m_market));
			PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), item);
		}
		PyObject* result = PyObject_CallMethod(static_cast<PyObject*>(m_provider), "quotes", "OO", list, nullptr == scan ? Py_False : Py_True);
		Py_DECREF(list);
		bool bOk = (nullptr != result) && (0 != PyList_Check(result));
		std::size_t validCount = 0;
		std::string invalidPrices;
		if (bOk)
		{
			for (Py_ssize_t i = 0; i < PyList_Size(result); ++i)
			{
				PyObject* row = PyList_GetItem(result, i);
				if (0 == PyDict_Check(row))
				{
					continue;
				}
				std::string strSymbol = String(row, "symbol");
				std::string strExchange = String(row, "exchange");
				const auto securityIter = securityByKey.find(strSymbol + ":" + strExchange);
				if (securityByKey.end() == securityIter)
				{
					continue;
				}
				bool stale = Py_True == PyDict_GetItemString(row, "stale");
				if (!stale)
				{
					++validCount;
				}
				std::int64_t now = NowMs();
				market::CQuote quote;
				quote.m_security = securityIter->second;
				securityByKey.erase(securityIter);
				quote.m_receive_time = Integer(row, "snapshot_time_ms");
				if (0 == quote.m_receive_time)
				{
					quote.m_receive_time = now;
				}
				quote.m_exchange_time = Integer(row, "exchange_time_ms");
				quote.m_last_price = Fixed(row, "price");
				quote.m_open_price = Fixed(row, "open");
				quote.m_high_price = Fixed(row, "high");
				quote.m_low_price = Fixed(row, "low");
				quote.m_pre_close = Fixed(row, "last_close");
				quote.m_volume = Integer(row, "vol");
				quote.m_turnover = Fixed(row, "amount", 2);
				quote.m_source = String(row, "source");
				quote.m_stale = stale;
				if (nullptr != scan)
				{
					if (0 >= quote.m_last_price)
					{
						if (!invalidPrices.empty())
						{
							invalidPrices += ", ";
						}
						invalidPrices += quote.m_security.String() + " (" + quote.m_source + ")";
						continue;
					}
					std::int64_t sourceTime = 0 < quote.m_exchange_time ? quote.m_exchange_time : quote.m_receive_time;
					bool currentDay = market::MarketDateIndex(sourceTime) == market::MarketDateIndex(now);
					if (!stale && currentDay)
					{
						scan->emplace_back(std::move(quote));
					}
					continue;
				}
				if (!stale)
				{
					std::chrono::steady_clock::time_point refreshed = std::chrono::steady_clock::now();
					std::lock_guard<std::mutex> lock(m_state_mutex);
					std::string key = quote.m_security.String();
					const auto mIter = m_last_refresh.find(key);
					if (m_last_refresh.end() != mIter)
					{
						m_refresh_intervals[m_interval_cursor] = std::chrono::duration_cast<std::chrono::milliseconds>(refreshed - mIter->second).count();
						m_interval_cursor = (m_interval_cursor + 1) % m_refresh_intervals.size();
						m_interval_count = (std::min)(m_interval_count + 1, m_refresh_intervals.size());
					}
					m_last_refresh.insert_or_assign(std::move(key), refreshed);
				}
				market::CDepth depth;
				depth.m_security = quote.m_security;
				depth.m_receive_time = quote.m_receive_time;
				depth.m_exchange_time = quote.m_exchange_time;
				depth.m_source = quote.m_source;
				depth.m_stale = stale;
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
					std::lock_guard<std::mutex> lck(m_state_mutex);
					quoteHandler = m_quote_handler;
					depthHandler = m_depth_handler;
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
			if (nullptr != PyErr_Occurred())
			{
				PrintPythonError("MooTdxProvider quote request failed");
			}
			else
			{
				CLogger::Write(Level::Error, "provider", "MooTdxProvider quote request failed: invalid Python result");
			}
		}
		Py_XDECREF(result);
		PyGILState_Release(gil);
		if (!invalidPrices.empty())
		{
			CLogger::Write(Level::Warning, "market", "Scan excluded invalid prices: ", invalidPrices);
		}
		if (nullptr == scan)
		{
			std::int64_t duration = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started).count();
			std::lock_guard<std::mutex> lock(m_state_mutex);
			m_request_durations[m_duration_cursor] = duration;
			m_duration_cursor = (m_duration_cursor + 1) % m_request_durations.size();
			m_duration_count = (std::min)(m_duration_count + 1, m_request_durations.size());
			if (validCount != securities.size())
			{
				++m_subscription_failures;
			}
		}
		return validCount;
	}
	void CMooTdxProvider::Wait(std::chrono::milliseconds duration)
	{
		std::unique_lock<std::mutex> lock(m_wait_mutex);
		m_wait_condition.wait_for(lock, duration, [this]()
		{
			return !m_running.load();
		});
	}

	void CMooTdxProvider::Stop()
	{
		{
			std::lock_guard<std::mutex> lock(m_wait_mutex);
			if (!m_running.exchange(false))
			{
				return;
			}
		}
		m_wait_condition.notify_all();
		if (m_thread.joinable())
		{
			m_thread.join();
		}
		PyGILState_STATE gil = PyGILState_Ensure();
		Py_XDECREF(static_cast<PyObject*>(m_provider));
		m_provider = nullptr;
		PyGILState_Release(gil);
	}
} // namespace provider
