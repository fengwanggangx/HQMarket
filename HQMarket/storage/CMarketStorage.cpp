#include "CMarketStorage.h"
#include <charconv>
#include <system_error>

namespace storage
{
	namespace
	{
		std::string Quote(const std::string& value)
		{
			std::string result("'");
			result.reserve(value.size() + 2);
			for (char ch : value)
			{
				result.push_back(ch);
				if (ch == '\'') result.push_back(ch);
			}
			result.push_back('\'');
			return result;
		}

		template <typename _Ty>
		bool ParseNumber(const std::string& value, _Ty& result)
		{
			const char* pBegin = value.data();
			const char* pEnd = pBegin + value.size();
			auto parsed = std::from_chars(pBegin, pEnd, result);
			return parsed.ec == std::errc{} && parsed.ptr == pEnd;
		}
	}

	CMarketStorage::~CMarketStorage()
	{
		Close();
	}

	bool CMarketStorage::Exec(const std::string& strSql)
	{
		return m_database.ExecUpdate(strSql) == 0;
	}

	bool CMarketStorage::Open(const std::filesystem::path& path)
	{
		std::lock_guard<std::mutex> lock(m_mtx_database);
		if (m_bOpen) return true;
		std::filesystem::create_directories(path.parent_path());
		db::CConnectParam param("", 0, "", "", path.string(), "");
		if (m_database.Connect(param) != 0) return false;
		m_bOpen = Exec("PRAGMA journal_mode=WAL;") && Exec("PRAGMA synchronous=NORMAL;") &&
			Exec("CREATE TABLE IF NOT EXISTS instrument(symbol TEXT NOT NULL, exchange INTEGER NOT NULL, name TEXT NOT NULL DEFAULT '', updated_at INTEGER NOT NULL, PRIMARY KEY(symbol, exchange));") &&
			Exec("CREATE TABLE IF NOT EXISTS bar(symbol TEXT NOT NULL, exchange INTEGER NOT NULL, channel INTEGER NOT NULL, begin_time INTEGER NOT NULL, open INTEGER NOT NULL, high INTEGER NOT NULL, low INTEGER NOT NULL, close INTEGER NOT NULL, volume INTEGER NOT NULL, turnover INTEGER NOT NULL, price_scale INTEGER NOT NULL, adjustment TEXT NOT NULL, source TEXT NOT NULL, PRIMARY KEY(symbol, exchange, channel, begin_time, adjustment));") &&
			Exec("CREATE TABLE IF NOT EXISTS sync_state(dataset TEXT PRIMARY KEY, cursor TEXT NOT NULL, updated_at INTEGER NOT NULL);");
		if (!m_bOpen) m_database.Close();
		return m_bOpen;
	}

	void CMarketStorage::Close()
	{
		std::lock_guard<std::mutex> lock(m_mtx_database);
		m_database.Close();
		m_bOpen = false;
	}

	bool CMarketStorage::IsOpen() const
	{
		std::lock_guard<std::mutex> lock(m_mtx_database);
		return m_bOpen;
	}

	bool CMarketStorage::UpsertBars(const std::vector<market::CBar>& bars)
	{
		std::lock_guard<std::mutex> lock(m_mtx_database);
		if (!m_bOpen || !m_database.BeginTransaction()) return false;
		bool bResult = true;
		for (const market::CBar& bar : bars)
		{
			std::string sql = "INSERT INTO bar(symbol,exchange,channel,begin_time,open,high,low,close,volume,turnover,price_scale,adjustment,source) VALUES(" +
				Quote(bar.m_instrument.m_strSymbol) + "," + std::to_string(static_cast<int>(bar.m_instrument.m_exchange)) + "," +
				std::to_string(static_cast<int>(bar.m_channel)) + "," + std::to_string(bar.m_nBeginTime) + "," +
				std::to_string(bar.m_nOpenPrice) + "," + std::to_string(bar.m_nHighPrice) + "," +
				std::to_string(bar.m_nLowPrice) + "," + std::to_string(bar.m_nClosePrice) + "," +
				std::to_string(bar.m_nVolume) + "," + std::to_string(bar.m_nTurnover) + "," +
				std::to_string(bar.m_nPriceScale) + "," + Quote(bar.m_strAdjustment) + "," + Quote(bar.m_strSource) +
				") ON CONFLICT(symbol,exchange,channel,begin_time,adjustment) DO UPDATE SET open=excluded.open,high=excluded.high,low=excluded.low,close=excluded.close,volume=excluded.volume,turnover=excluded.turnover,price_scale=excluded.price_scale,source=excluded.source";
			if (!Exec(sql))
			{
				bResult = false;
				break;
			}
		}
		if (bResult && m_database.EndTransaction()) return true;
		m_database.RollBackTransaction();
		return false;
	}

	std::vector<market::CBar> CMarketStorage::QueryBars(const market::CInstrument& instrument, market::Channel channel,
		std::int64_t nBeginTime, std::int64_t nEndTime)
	{
		std::lock_guard<std::mutex> lock(m_mtx_database);
		std::vector<market::CBar> result;
		if (!m_bOpen) return result;
		std::string sql = "SELECT begin_time,open,high,low,close,volume,turnover,price_scale,adjustment,source FROM bar WHERE symbol=" +
			Quote(instrument.m_strSymbol) + " AND exchange=" + std::to_string(static_cast<int>(instrument.m_exchange)) +
			" AND channel=" + std::to_string(static_cast<int>(channel)) + " AND begin_time BETWEEN " +
			std::to_string(nBeginTime) + " AND " + std::to_string(nEndTime) + " ORDER BY begin_time";
		const db::_TyRows& rows = m_database.ExecQuery(sql).second;
		result.reserve(rows.size());
		for (const db::_TyRows::value_type& row : rows)
		{
			if (row.size() != 10) continue;
			market::CBar bar;
			bar.m_instrument = instrument;
			bar.m_channel = channel;
			if (!ParseNumber(row[0], bar.m_nBeginTime) || !ParseNumber(row[1], bar.m_nOpenPrice) ||
				!ParseNumber(row[2], bar.m_nHighPrice) || !ParseNumber(row[3], bar.m_nLowPrice) ||
				!ParseNumber(row[4], bar.m_nClosePrice) || !ParseNumber(row[5], bar.m_nVolume) ||
				!ParseNumber(row[6], bar.m_nTurnover) || !ParseNumber(row[7], bar.m_nPriceScale)) continue;
			bar.m_strAdjustment = row[8];
			bar.m_strSource = row[9];
			result.emplace_back(std::move(bar));
		}
		return result;
	}
} // namespace storage
