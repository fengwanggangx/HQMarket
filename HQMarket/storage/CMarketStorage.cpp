#include "CMarketStorage.h"
#include <sqlite3.h>
namespace storage
{
	CMarketStorage::~CMarketStorage()
	{
		Close();
	}
	bool CMarketStorage::Exec(const char* pszSql)
	{
		return sqlite3_exec(m_pDatabase, pszSql, nullptr, nullptr, nullptr) == SQLITE_OK;
	}
	bool CMarketStorage::Open(const std::filesystem::path& path)
	{
		std::lock_guard lock(m_mtxDatabase);
		if (m_pDatabase != nullptr)
		{
			return true;
		}
		std::filesystem::create_directories(path.parent_path());
		if (sqlite3_open_v2(path.string().c_str(), &m_pDatabase,
							SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
		{
			if (m_pDatabase != nullptr)
			{
				sqlite3_close(m_pDatabase);
			}
			m_pDatabase = nullptr;
			return false;
		}
		return Exec("PRAGMA journal_mode=WAL;") && Exec("PRAGMA synchronous=NORMAL;") &&
			   Exec("CREATE TABLE IF NOT EXISTS instrument(symbol TEXT NOT NULL, exchange INTEGER NOT NULL, name TEXT NOT "
					"NULL DEFAULT '', updated_at INTEGER NOT NULL, PRIMARY KEY(symbol, exchange));") &&
			   Exec("CREATE TABLE IF NOT EXISTS bar(symbol TEXT NOT NULL, exchange INTEGER NOT NULL, channel INTEGER NOT "
					"NULL, begin_time INTEGER NOT NULL, open INTEGER NOT NULL, high INTEGER NOT NULL, low INTEGER NOT "
					"NULL, close INTEGER NOT NULL, volume INTEGER NOT NULL, turnover INTEGER NOT NULL, price_scale INTEGER "
					"NOT NULL, adjustment TEXT NOT NULL, source TEXT NOT NULL, PRIMARY KEY(symbol, exchange, channel, "
					"begin_time, adjustment));") &&
			   Exec("CREATE TABLE IF NOT EXISTS sync_state(dataset TEXT PRIMARY KEY, cursor TEXT NOT NULL, updated_at "
					"INTEGER NOT NULL);");
	}
	void CMarketStorage::Close()
	{
		std::lock_guard lock(m_mtxDatabase);
		if (m_pDatabase != nullptr)
		{
			sqlite3_close(m_pDatabase);
			m_pDatabase = nullptr;
		}
	}
	bool CMarketStorage::IsOpen() const
	{
		std::lock_guard lock(m_mtxDatabase);
		return m_pDatabase != nullptr;
	}
	bool CMarketStorage::UpsertBars(const std::vector<market::CBar>& bars)
	{
		std::lock_guard lock(m_mtxDatabase);
		if (!m_pDatabase || sqlite3_exec(m_pDatabase, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
		{
			return false;
		}
		const char* sql =
			"INSERT INTO "
			"bar(symbol,exchange,channel,begin_time,open,high,low,close,volume,turnover,price_scale,adjustment,source) "
			"VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(symbol,exchange,channel,begin_time,adjustment) DO UPDATE SET "
			"open=excluded.open,high=excluded.high,low=excluded.low,close=excluded.close,volume=excluded.volume,turnover="
			"excluded.turnover,price_scale=excluded.price_scale,source=excluded.source";
		sqlite3_stmt* statement = nullptr;
		bool ok = sqlite3_prepare_v2(m_pDatabase, sql, -1, &statement, nullptr) == SQLITE_OK;
		for (const auto& bar : bars)
		{
			if (ok == false)
			{
				break;
			}
			sqlite3_reset(statement);
			sqlite3_clear_bindings(statement);
			sqlite3_bind_text(statement, 1, bar.m_instrument.m_strSymbol.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(statement, 2, static_cast<int>(bar.m_instrument.m_exchange));
			sqlite3_bind_int(statement, 3, static_cast<int>(bar.m_channel));
			sqlite3_bind_int64(statement, 4, bar.m_nBeginTime);
			sqlite3_bind_int64(statement, 5, bar.m_nOpenPrice);
			sqlite3_bind_int64(statement, 6, bar.m_nHighPrice);
			sqlite3_bind_int64(statement, 7, bar.m_nLowPrice);
			sqlite3_bind_int64(statement, 8, bar.m_nClosePrice);
			sqlite3_bind_int64(statement, 9, bar.m_nVolume);
			sqlite3_bind_int64(statement, 10, bar.m_nTurnover);
			sqlite3_bind_int(statement, 11, bar.m_nPriceScale);
			sqlite3_bind_text(statement, 12, bar.m_strAdjustment.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(statement, 13, bar.m_strSource.c_str(), -1, SQLITE_TRANSIENT);
			ok = sqlite3_step(statement) == SQLITE_DONE;
		}
		if (statement != nullptr)
		{
			sqlite3_finalize(statement);
		}
		sqlite3_exec(m_pDatabase, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
		return ok;
	}
	std::vector<market::CBar> CMarketStorage::QueryBars(const market::CInstrument& instrument, market::Channel channel,
														std::int64_t nBeginTime, std::int64_t nEndTime)
	{
		std::lock_guard lock(m_mtxDatabase);
		std::vector<market::CBar> result;
		if (m_pDatabase == nullptr)
		{
			return result;
		}
		const char* sql = "SELECT begin_time,open,high,low,close,volume,turnover,price_scale,adjustment,source FROM bar "
						  "WHERE symbol=? AND exchange=? AND channel=? AND begin_time BETWEEN ? AND ? ORDER BY begin_time";
		sqlite3_stmt* statement = nullptr;
		if (sqlite3_prepare_v2(m_pDatabase, sql, -1, &statement, nullptr) != SQLITE_OK)
		{
			return result;
		}
		sqlite3_bind_text(statement, 1, instrument.m_strSymbol.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(statement, 2, static_cast<int>(instrument.m_exchange));
		sqlite3_bind_int(statement, 3, static_cast<int>(channel));
		sqlite3_bind_int64(statement, 4, nBeginTime);
		sqlite3_bind_int64(statement, 5, nEndTime);
		while (sqlite3_step(statement) == SQLITE_ROW)
		{
			market::CBar bar;
			bar.m_instrument = instrument;
			bar.m_channel = channel;
			bar.m_nBeginTime = sqlite3_column_int64(statement, 0);
			bar.m_nOpenPrice = sqlite3_column_int64(statement, 1);
			bar.m_nHighPrice = sqlite3_column_int64(statement, 2);
			bar.m_nLowPrice = sqlite3_column_int64(statement, 3);
			bar.m_nClosePrice = sqlite3_column_int64(statement, 4);
			bar.m_nVolume = sqlite3_column_int64(statement, 5);
			bar.m_nTurnover = sqlite3_column_int64(statement, 6);
			bar.m_nPriceScale = sqlite3_column_int(statement, 7);
			bar.m_strAdjustment = reinterpret_cast<const char*>(sqlite3_column_text(statement, 8));
			bar.m_strSource = reinterpret_cast<const char*>(sqlite3_column_text(statement, 9));
			result.emplace_back(std::move(bar));
		}
		sqlite3_finalize(statement);
		return result;
	}
} // namespace storage
