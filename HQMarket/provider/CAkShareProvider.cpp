#include "CAkShareProvider.h"
#include <Python.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
namespace provider
{
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
		std::time_t value = static_cast<std::time_t>((nMilliseconds + ChinaTimeOffsetMilliseconds) / 1000);
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
	static std::int64_t Fixed(PyObject* row, const char* name, int scale)
	{
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
		return static_cast<std::int64_t>(std::llround(fValue * std::pow(10.0, scale)));
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
		security.m_strCode = strCode;
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
				instrument.m_security.m_strCode = symbol;
				if (nullptr != nameValue)
				{
					const char* pName = PyUnicode_AsUTF8(nameValue);
					if (nullptr != pName)
					{
						instrument.m_strName = pName;
					}
				}
				instrument.m_pinyinFullAliases = StringList(row, "pinyin_full_aliases");
				instrument.m_pinyinShortAliases = StringList(row, "pinyin_short_aliases");
				if (instrument.m_security.m_strCode.starts_with('6'))
				{
					instrument.m_security.m_market = market::Exchange::sse;
				}
				else if (instrument.m_security.m_strCode.starts_with('0') || instrument.m_security.m_strCode.starts_with('3'))
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
	bool CAkShareProvider::Subscribe(const std::vector<market::CChannelInfo>&)
	{
		return false;
	}
	bool CAkShareProvider::Unsubscribe(const std::vector<market::CChannelInfo>&)
	{
		return false;
	}
	std::vector<market::CBar> CAkShareProvider::QueryBars(const market::CSecurity& security, market::Channel channel,
														  std::int64_t nBeginTime, std::int64_t nEndTime)
	{
		std::vector<market::CBar> bars;
		std::unique_lock<std::mutex> lck(m_mtx_state);
		if (((market::Channel::bar_1d != channel) && (market::Channel::bar_1m != channel)) || (nullptr == m_pProvider))
		{
			return bars;
		}
		bool bMinute = market::Channel::bar_1m == channel;
		std::string begin = bMinute ? DateTimeString(nBeginTime) : DateString(nBeginTime);
		std::string end = bMinute ? DateTimeString(nEndTime) : DateString(nEndTime);
		PyGILState_STATE gil = PyGILState_Ensure();
		PyObject* result = PyObject_CallMethod(static_cast<PyObject*>(m_pProvider), bMinute ? "minute_bars" : "daily_bars", "ssss",
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
	std::vector<market::CInstrument> CAkShareProvider::QueryInstruments() const
	{
		std::lock_guard<std::mutex> lck(m_mtx_state);
		return m_instruments;
	}
	std::vector<market::CSector> CAkShareProvider::QuerySectors(market::SectorType type)
	{
		std::lock_guard<std::mutex> lck(m_mtx_state);
		if ((market::SectorType::industry != type) || (nullptr == m_pProvider))
		{
			return {};
		}
		std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		if (!m_sectors.empty() && (now < m_sectorExpiry))
		{
			return m_sectors;
		}
		PyGILState_STATE gil = PyGILState_Ensure();
		PyObject* result = PyObject_CallMethod(static_cast<PyObject*>(m_pProvider), "industry_sectors", nullptr);
		bool bOk = (nullptr != result) && (0 != PyList_Check(result));
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
				sector.m_strCode = String(row, "code");
				sector.m_strName = String(row, "name");
				if (sector.m_strCode.empty() || sector.m_strName.empty())
				{
					continue;
				}
				sector.m_nChangePercent = Fixed(row, "change_percent", sector.m_nPercentScale);
				sector.m_nRisingCount = Integer(row, "rising_count");
				sector.m_nFallingCount = Integer(row, "falling_count");
				sector.m_nFlatCount = Integer(row, "flat_count");
				sector.m_nMemberCount = Integer(row, "member_count");
				sector.m_strLeadingName = String(row, "leading_name");
				sector.m_leadingSecurity = Security(String(row, "leading_symbol"));
				sector.m_nSnapshotTime = nSnapshotTime;
				sectors.emplace_back(std::move(sector));
			}
			bOk = !sectors.empty();
		}
		if (!bOk)
		{
			PyErr_Clear();
		}
		Py_XDECREF(result);
		PyGILState_Release(gil);
		if (bOk)
		{
			m_sectors = std::move(sectors);
			m_sectorExpiry = now + std::chrono::seconds(60);
		}
		m_status = { bOk, bOk ? "ready" : "sector request failed" };
		return bOk ? m_sectors : std::vector<market::CSector>{};
	}
	market::CSectorConstituents CAkShareProvider::QuerySectorConstituents(market::SectorType type, const std::string& strSectorCode)
	{
		std::lock_guard<std::mutex> lck(m_mtx_state);
		market::CSectorConstituents value;
		if ((market::SectorType::industry != type) || strSectorCode.empty() || (nullptr == m_pProvider))
		{
			return value;
		}
		std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		const auto cacheIter = m_sectorConstituents.find(strSectorCode);
		const auto expiryIter = m_sectorConstituentExpiry.find(strSectorCode);
		if ((m_sectorConstituents.end() != cacheIter) && (m_sectorConstituentExpiry.end() != expiryIter) && (now < expiryIter->second))
		{
			return cacheIter->second;
		}
		const auto sectorIter = std::find_if(m_sectors.begin(), m_sectors.end(), [&strSectorCode](const market::CSector& sector)
											 { return strSectorCode == sector.m_strCode; });
		if (m_sectors.end() == sectorIter)
		{
			return value;
		}
		value.m_sector = *sectorIter;
		PyGILState_STATE gil = PyGILState_Ensure();
		PyObject* result = PyObject_CallMethod(static_cast<PyObject*>(m_pProvider), "industry_constituents", "s", sectorIter->m_strName.c_str());
		bool bOk = (nullptr != result) && (0 != PyList_Check(result));
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
				instrument.m_strName = String(row, "name");
				const auto instrumentIter = std::find_if(m_instruments.begin(), m_instruments.end(), [&instrument](const market::CInstrument& current)
				{
					return current.m_security == instrument.m_security;
				});
				if (m_instruments.end() != instrumentIter)
				{
					instrument.m_pinyinFullAliases = instrumentIter->m_pinyinFullAliases;
					instrument.m_pinyinShortAliases = instrumentIter->m_pinyinShortAliases;
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
			PyErr_Clear();
		}
		Py_XDECREF(result);
		PyGILState_Release(gil);
		if (bOk)
		{
			value.m_nSnapshotTime = NowMilliseconds();
			value.m_sector.m_nMemberCount = static_cast<int>(value.m_securities.size());
			m_sectorConstituents.insert_or_assign(strSectorCode, value);
			m_sectorConstituentExpiry.insert_or_assign(strSectorCode, now + std::chrono::minutes(5));
		}
		m_status = { bOk, bOk ? "ready" : "sector constituent request failed" };
		return bOk ? value : market::CSectorConstituents{};
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
