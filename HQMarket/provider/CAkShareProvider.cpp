#include "../log/CLogger.h"
#include "CAkShareProvider.h"
#include <Python.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <iostream>
#include <sstream>
namespace provider
{
	static std::string LogPythonFailure(const char* source)
	{
		if (nullptr == PyErr_Occurred())
		{
			std::string failure = std::string("akshare ") + source + " failed: empty or invalid result";
			CLogger::Write(Level::Error, "provider", failure);
			return failure;
		}
		PyObject* errorType = nullptr;
		PyObject* errorValue = nullptr;
		PyObject* errorTraceback = nullptr;
		PyErr_Fetch(&errorType, &errorValue, &errorTraceback);
		PyErr_NormalizeException(&errorType, &errorValue, &errorTraceback);
		PyObject* errorText = nullptr != errorValue ? PyObject_Str(errorValue) : nullptr;
		const char* name = nullptr != errorType ? PyExceptionClass_Name(errorType) : nullptr;
		const char* message = nullptr != errorText ? PyUnicode_AsUTF8(errorText) : nullptr;
		std::string failure = std::string("akshare ") + source + " failed: " + (nullptr != name ? name : "invalid result");
		if (nullptr != message)
		{
			failure += ": ";
			failure += message;
		}
		CLogger::Write(Level::Error, "provider", failure);
		Py_XDECREF(errorText);
		Py_XDECREF(errorTraceback);
		Py_XDECREF(errorValue);
		Py_XDECREF(errorType);
		PyErr_Clear();
		return failure;
	}

	constexpr std::int64_t ChinaTimeOffsetMilliseconds = 8LL * 60LL * 60LL * 1000LL;

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
	static std::string DateTimeString(std::int64_t nMilliseconds)
	{
		if (0 >= nMilliseconds)
		{
			return "1990-01-01 00:00:00";
		}
		std::time_t value = static_cast<std::time_t>(nMilliseconds / 1000 + ChinaTimeOffsetMilliseconds / 1000);
		std::tm date{};
#ifdef _WIN32
		gmtime_s(&date, &value);
#else
		gmtime_r(&value, &date);
#endif
		std::ostringstream out;
		out << std::put_time(&date, "%Y-%m-%d %H:%M:%S");
		return out.str();
	}
	static std::int64_t DateMilliseconds(const char* value)
	{
		std::tm date{};
		std::string strValue = nullptr != value ? value : "";
		bool bHasTime = std::string::npos != strValue.find(' ');
		std::istringstream input(strValue);
		input >> std::get_time(&date, bHasTime ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d");
		if (input.fail())
		{
			return 0;
		}
#ifdef _WIN32
		std::int64_t nResult = static_cast<std::int64_t>(_mkgmtime(&date)) * 1000;
#else
		std::int64_t nResult = static_cast<std::int64_t>(timegm(&date)) * 1000;
#endif
		return bHasTime ? nResult - ChinaTimeOffsetMilliseconds : nResult;
	}
	static std::int64_t Fixed(PyObject* row, const char* name, int scale, bool* available = nullptr)
	{
		if (nullptr != available)
		{
			*available = false;
		}
		PyObject* value = PyDict_GetItemString(row, name);
		if (nullptr == value)
		{
			return 0;
		}
		PyObject* number = PyNumber_Float(value);
		if (nullptr == number)
		{
			PyErr_Clear();
			return 0;
		}
		double fValue = PyFloat_AsDouble(number);
		Py_DECREF(number);
		double scaled = fValue * std::pow(10.0, scale);
		double upperBound = static_cast<double>((std::numeric_limits<std::int64_t>::max)());
		if (!std::isfinite(scaled) || scaled < -upperBound || upperBound <= scaled)
		{
			return 0;
		}
		if (nullptr != available)
		{
			*available = true;
		}
		return static_cast<std::int64_t>(std::llround(scaled));
	}
	static int Integer(PyObject* row, const char* name)
	{
		return static_cast<int>(Fixed(row, name, 0));
	}
	static std::string String(PyObject* row, const char* name)
	{
		PyObject* value = PyDict_GetItemString(row, name);
		const char* pValue = nullptr != value ? PyUnicode_AsUTF8(value) : nullptr;
		if (nullptr == pValue)
		{
			PyErr_Clear();
			return {};
		}
		return pValue;
	}
	static std::vector<std::string> StringList(PyObject* row, const char* name)
	{
		std::vector<std::string> result;
		PyObject* values = PyDict_GetItemString(row, name);
		if ((nullptr == values) || (0 == PyList_Check(values)))
		{
			return result;
		}
		result.reserve(static_cast<std::size_t>(PyList_Size(values)));
		for (Py_ssize_t nIndex = 0; PyList_Size(values) > nIndex; ++nIndex)
		{
			PyObject* value = PyList_GetItem(values, nIndex);
			const char* pValue = nullptr != value ? PyUnicode_AsUTF8(value) : nullptr;
			if (nullptr != pValue)
			{
				result.emplace_back(pValue);
			}
			else
			{
				PyErr_Clear();
			}
		}
		return result;
	}
	static market::CSecurity Security(const std::string& strCode)
	{
		market::CSecurity security;
		security.m_code = strCode;
		if (strCode.starts_with('6'))
		{
			security.m_market = market::Exchange::sse;
		}
		else if (strCode.starts_with('0') || strCode.starts_with('3'))
		{
			security.m_market = market::Exchange::szse;
		}
		else
		{
			security.m_market = market::Exchange::bse;
		}
		return security;
	}
	static std::int64_t NowMilliseconds()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
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
		std::string failure;
		if (!bOk)
		{
			failure = LogPythonFailure("provider initialization");
		}
		Py_XDECREF(type);
		Py_XDECREF(module);
		if (bOk)
		{
			m_provider = instance;
		}
		PyObject* values = bOk ? PyObject_CallMethod(instance, "instruments", nullptr) : nullptr;
		if ((nullptr != values) && (0 != PyList_Check(values)))
		{
			for (Py_ssize_t i = 0; i < PyList_Size(values); ++i)
			{
				PyObject* row = PyList_GetItem(values, i);
				PyObject* symbolValue = PyDict_Check(row) ? PyDict_GetItemString(row, "symbol") : nullptr;
				PyObject* nameValue = PyDict_Check(row) ? PyDict_GetItemString(row, "name") : nullptr;
				if (nullptr == symbolValue)
				{
					continue;
				}
				const char* symbol = PyUnicode_AsUTF8(symbolValue);
				if (nullptr == symbol)
				{
					continue;
				}
				market::CInstrument instrument;
				instrument.m_security.m_code = symbol;
				if (nullptr != nameValue)
				{
					const char* pName = PyUnicode_AsUTF8(nameValue);
					if (nullptr != pName)
					{
						instrument.m_name = pName;
					}
				}
				instrument.m_pinyin_full_aliases = StringList(row, "pinyin_full_aliases");
				instrument.m_pinyin_short_aliases = StringList(row, "pinyin_short_aliases");
				std::string strExchange = String(row, "exchange");
				if ("HKEX" == strExchange)
				{
					instrument.m_security.m_market = market::Exchange::hkex;
				}
				else if ("SSE" == strExchange || instrument.m_security.m_code.starts_with('6'))
				{
					instrument.m_security.m_market = market::Exchange::sse;
				}
				else if ("SZSE" == strExchange || instrument.m_security.m_code.starts_with('0') || instrument.m_security.m_code.starts_with('3'))
				{
					instrument.m_security.m_market = market::Exchange::szse;
				}
				else
				{
					instrument.m_security.m_market = market::Exchange::bse;
				}
				m_instruments.emplace_back(std::move(instrument));
			}
		}
		if (bOk && (nullptr == values))
		{
			failure = LogPythonFailure("stock_info_a_code_name");
			bOk = false;
		}
		else if ((nullptr != values) && (0 == PyList_Check(values)))
		{
			failure = LogPythonFailure("stock_info_a_code_name result");
			bOk = false;
		}
		Py_XDECREF(values);
		PyGILState_Release(gil);
		std::lock_guard<std::mutex> lck(m_state_mutex);
		SetStatus({ bOk, bOk ? "ready" : failure });
		return bOk;
	}
	bool CAkShareProvider::Subscribe(const std::vector<market::CChannelInfo>&)
	{
		return false;
	}
	bool CAkShareProvider::Unsubscribe(const std::vector<market::CChannelInfo>&)
	{
		return false;
	}
	std::vector<market::CBar> CAkShareProvider::QueryBars(const market::CSecurity& security, market::Channel channel,
		std::int64_t nBeginTime, std::int64_t nEndTime, market::CProviderStatus* queryStatus)
	{
		std::vector<market::CBar> bars;
		std::unique_lock<std::mutex> lck(m_state_mutex);
		if (((market::Channel::bar_1d != channel) && (market::Channel::bar_1m != channel)) || (nullptr == m_provider))
		{
			if (nullptr != queryStatus)
			{
				*queryStatus = { false, "history provider unavailable for this request" };
			}
			return bars;
		}
		bool bMinute = market::Channel::bar_1m == channel;
		std::string begin = bMinute ? DateTimeString(nBeginTime) : DateString(nBeginTime);
		std::string end = bMinute ? DateTimeString(nEndTime) : DateString(nEndTime);
		PyGILState_STATE gil = PyGILState_Ensure();
		std::string strExchange = market::GetMarketString(security.m_market);
		PyObject* result = PyObject_CallMethod(static_cast<PyObject*>(m_provider), bMinute ? "minute_bars" : "daily_bars", "sssss",
			security.m_code.c_str(), begin.c_str(), end.c_str(), "", strExchange.c_str());
		bool bOk = (nullptr != result) && (0 != PyList_Check(result));
		std::string failure;
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
				bar.m_begin_time = DateMilliseconds(nullptr != date ? PyUnicode_AsUTF8(date) : nullptr);
				bar.m_open_price = Fixed(row, "open", 4);
				bar.m_high_price = Fixed(row, "high", 4);
				bar.m_low_price = Fixed(row, "low", 4);
				bar.m_close_price = Fixed(row, "close", 4);
				if (0 >= bar.m_open_price || 0 >= bar.m_high_price ||
					0 >= bar.m_low_price || 0 >= bar.m_close_price)
				{
					continue;
				}
				bar.m_volume = Fixed(row, "volume", 0);
				bar.m_turnover = Fixed(row, "turnover", 2, &bar.m_hasTurnover);
				bar.m_source = String(row, "source");
				if (bar.m_source.empty())
				{
					bar.m_source = "akshare";
				}
				if (0 == bar.m_begin_time || bar.m_begin_time < nBeginTime || nEndTime < bar.m_begin_time)
				{
					continue;
				}
				PyObject* completed = PyDict_GetItemString(row, "completed");
				bar.m_completed = (nullptr != completed) && PyBool_Check(completed) && (1 == PyObject_IsTrue(completed));
				bar.m_end_time = Fixed(row, "end_time_ms", 0);
				bar.m_basis_version = String(row, "basis_version");
				bar.m_data_version = static_cast<std::uint64_t>(std::max<std::int64_t>(0, Fixed(row, "data_version", 0)));
				bars.emplace_back(std::move(bar));
			}
		}
		if (!bOk)
		{
			failure = LogPythonFailure(bMinute ? "Eastmoney minute history" : "Eastmoney stock_zh_a_hist / Tencent daily history");
		}
		Py_XDECREF(result);
		PyGILState_Release(gil);
		market::CProviderStatus status{ bOk, bOk ? "ready" : failure };
		if (nullptr != queryStatus)
		{
			*queryStatus = status;
		}
		SetStatus(std::move(status));
		return bars;
	}
	void CAkShareProvider::SetStatus(market::CProviderStatus status)
	{
		std::lock_guard<std::mutex> lck(m_mtx_status);
		m_status = std::move(status);
	}
	market::CProviderStatus CAkShareProvider::GetStatus() const
	{
		std::lock_guard<std::mutex> lck(m_mtx_status);
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
		std::lock_guard<std::mutex> lck(m_state_mutex);
		return m_instruments;
	}
	std::vector<market::CSector> CAkShareProvider::QuerySectors(market::SectorType type)
	{
		std::lock_guard<std::mutex> lck(m_state_mutex);
		if ((market::SectorType::industry != type) || (nullptr == m_provider))
		{
			return {};
		}
		std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		if (!m_sectors.empty() && (now < m_sector_expiry))
		{
			return m_sectors;
		}
		PyGILState_STATE gil = PyGILState_Ensure();
		PyObject* result = PyObject_CallMethod(static_cast<PyObject*>(m_provider), "industry_sectors", nullptr);
		bool bOk = (nullptr != result) && (0 != PyList_Check(result));
		std::string failure;
		std::vector<market::CSector> sectors;
		if (bOk)
		{
			sectors.reserve(static_cast<std::size_t>(PyList_Size(result)));
			std::int64_t nSnapshotTime = NowMilliseconds();
			for (Py_ssize_t nIndex = 0; nIndex < PyList_Size(result); ++nIndex)
			{
				PyObject* row = PyList_GetItem(result, nIndex);
				if (0 == PyDict_Check(row))
				{
					continue;
				}
				market::CSector sector;
				sector.m_type = type;
				sector.m_code = String(row, "code");
				sector.m_name = String(row, "name");
				if (sector.m_code.empty() || sector.m_name.empty())
				{
					continue;
				}
				sector.m_change_percent = Fixed(row, "change_percent", sector.m_percent_scale);
				sector.m_rising_count = Integer(row, "rising_count");
				sector.m_falling_count = Integer(row, "falling_count");
				sector.m_flat_count = Integer(row, "flat_count");
				sector.m_member_count = Integer(row, "member_count");
				sector.m_leading_name = String(row, "leading_name");
				sector.m_leading_security = Security(String(row, "leading_symbol"));
				sector.m_snapshot_time = nSnapshotTime;
				sectors.emplace_back(std::move(sector));
			}
			bOk = !sectors.empty();
		}
		if (!bOk)
		{
			failure = LogPythonFailure("Eastmoney stock_board_industry_name_em (17.push2.eastmoney.com)");
		}
		Py_XDECREF(result);
		PyGILState_Release(gil);
		if (bOk)
		{
			m_sectors = std::move(sectors);
			m_sector_expiry = now + std::chrono::seconds(60);
		}
		SetStatus({ bOk, bOk ? "ready" : failure });
		return bOk ? m_sectors : std::vector<market::CSector>{};
	}
	market::CSectorConstituents CAkShareProvider::QuerySectorConstituents(market::SectorType type, const std::string& strSectorCode)
	{
		std::lock_guard<std::mutex> lck(m_state_mutex);
		market::CSectorConstituents value;
		if ((market::SectorType::industry != type) || strSectorCode.empty() || (nullptr == m_provider))
		{
			return value;
		}
		std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		const auto cacheIter = m_sector_constituents.find(strSectorCode);
		const auto expiryIter = m_sector_constituent_expiry.find(strSectorCode);
		if ((m_sector_constituents.end() != cacheIter) && (m_sector_constituent_expiry.end() != expiryIter) && (now < expiryIter->second))
		{
			return cacheIter->second;
		}
		const auto sectorIter = std::find_if(m_sectors.begin(), m_sectors.end(), [&strSectorCode](const market::CSector& sector)
		{
			return strSectorCode == sector.m_code;
		});
		if (m_sectors.end() == sectorIter)
		{
			return value;
		}
		value.m_sector = *sectorIter;
		PyGILState_STATE gil = PyGILState_Ensure();
		PyObject* result = PyObject_CallMethod(static_cast<PyObject*>(m_provider), "industry_constituents", "s", sectorIter->m_name.c_str());
		bool bOk = (nullptr != result) && (0 != PyList_Check(result));
		std::string failure;
		if (bOk)
		{
			value.m_securities.reserve(static_cast<std::size_t>(PyList_Size(result)));
			for (Py_ssize_t nIndex = 0; nIndex < PyList_Size(result); ++nIndex)
			{
				PyObject* row = PyList_GetItem(result, nIndex);
				if (0 == PyDict_Check(row))
				{
					continue;
				}
				market::CInstrument instrument;
				instrument.m_security = Security(String(row, "symbol"));
				instrument.m_name = String(row, "name");
				const auto instrumentIter = std::find_if(m_instruments.begin(), m_instruments.end(), [&instrument](const market::CInstrument& current)
				{
					return current.m_security == instrument.m_security;
				});
				if (m_instruments.end() != instrumentIter)
				{
					instrument.m_pinyin_full_aliases = instrumentIter->m_pinyin_full_aliases;
					instrument.m_pinyin_short_aliases = instrumentIter->m_pinyin_short_aliases;
				}
				if (instrument.m_security.IsValid())
				{
					value.m_securities.emplace_back(std::move(instrument));
				}
			}
			bOk = !value.m_securities.empty();
		}
		if (!bOk)
		{
			failure = LogPythonFailure("Eastmoney stock_board_industry_cons_em");
		}
		Py_XDECREF(result);
		PyGILState_Release(gil);
		if (bOk)
		{
			value.m_snapshot_time = NowMilliseconds();
			value.m_sector.m_member_count = static_cast<int>(value.m_securities.size());
			m_sector_constituents.insert_or_assign(strSectorCode, value);
			m_sector_constituent_expiry.insert_or_assign(strSectorCode, now + std::chrono::minutes(5));
		}
		SetStatus({ bOk, bOk ? "ready" : failure });
		return bOk ? value : market::CSectorConstituents{};
	}
	void CAkShareProvider::Stop()
	{
		std::lock_guard<std::mutex> lck(m_state_mutex);
		if ((nullptr == m_provider) || (0 == Py_IsInitialized()))
		{
			return;
		}
		PyGILState_STATE gil = PyGILState_Ensure();
		Py_DECREF(static_cast<PyObject*>(m_provider));
		m_provider = nullptr;
		PyGILState_Release(gil);
		SetStatus({ false, "stopped" });
	}
} // namespace provider
