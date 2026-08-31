#include "CHQRecorder.h"
#include "../common/utility.h"
#include "../database/IDataBase.h"
#include <charconv>
#include <system_error>

namespace
{
	std::string Quote(const std::string& value)
	{
		std::string result("'");
		result.reserve(value.size() + 2);
		for (char ch : value)
		{
			result.push_back(ch);
			if ('\'' == ch)
			{
				result.push_back(ch);
			}
		}
		result.push_back('\'');
		return result;
	}
}

	CHQRecorder::~CHQRecorder()
	{
		Close();
	}

	bool CHQRecorder::Exec(const std::string& strSQL)
	{
		if (nullptr == m_db)
		{
			return false;
		}
		return 0 == m_db->ExecUpdate(strSQL);
	}

	bool CHQRecorder::Open(const std::filesystem::path& path)
	{
		CDBEngine* pDBEngine = CDBEngine::InstancePtr();
		if (nullptr == pDBEngine)
		{
			return false;
		}

		std::unique_lock<std::shared_mutex> lck(m_mtx_db);
		if (m_bOpen)
		{
			return true;
		}
		std::filesystem::create_directories(path.parent_path());
		m_db = pDBEngine->GetDBPtr(db::em_database::sqlite);
		if (nullptr == m_db)
		{
			db::CConnectParam param("", 0, "", "", path.string(), "");
			if (0 != pDBEngine->Initialize(db::em_database::sqlite, param))
			{
				return false;
			}
			m_db = pDBEngine->GetDBPtr(db::em_database::sqlite);
		}
		if (nullptr == m_db)
		{
			return false;
		}

		m_bOpen = Exec("PRAGMA journal_mode=WAL;") && 
			Exec("PRAGMA synchronous=NORMAL;") &&
			Exec("CREATE TABLE IF NOT EXISTS instrument(symbol TEXT NOT NULL, exchange INTEGER NOT NULL, name TEXT NOT NULL DEFAULT '', updated_at INTEGER NOT NULL, PRIMARY KEY(symbol, exchange));") &&
			Exec("CREATE TABLE IF NOT EXISTS bar(symbol TEXT NOT NULL, exchange INTEGER NOT NULL, channel INTEGER NOT NULL, begin_time INTEGER NOT NULL, open INTEGER NOT NULL, high INTEGER NOT NULL, low INTEGER NOT NULL, close INTEGER NOT NULL, volume INTEGER NOT NULL, turnover INTEGER NOT NULL, price_scale INTEGER NOT NULL, adjustment TEXT NOT NULL, source TEXT NOT NULL, PRIMARY KEY(symbol, exchange, channel, begin_time, adjustment));") &&
			Exec("CREATE TABLE IF NOT EXISTS sync_state(dataset TEXT PRIMARY KEY, cursor TEXT NOT NULL, updated_at INTEGER NOT NULL);");
		
		if (!m_bOpen)
		{
			m_db.reset();
		}
		return m_bOpen;
	}

	void CHQRecorder::Close()
	{
		std::unique_lock<std::shared_mutex> lck(m_mtx_db);
		m_db.reset();
		m_bOpen = false;
	}

	bool CHQRecorder::IsOpen() const
	{
		std::shared_lock<std::shared_mutex> lck(m_mtx_db);
		return m_bOpen && (nullptr != m_db);
	}

	bool CHQRecorder::UpsertBars(const std::vector<market::CBar>& bars)
	{
		std::unique_lock<std::shared_mutex> lck(m_mtx_db);
		if (!m_bOpen || (nullptr == m_db))
		{
			return false;
		}

		if (!m_db->BeginTransaction())
		{
			return false;
		}
		bool bOk = true;
		for (const market::CBar& bar : bars)
		{
			std::string sql = "INSERT INTO bar(symbol,exchange,channel,begin_time,open,high,low,close,volume,turnover,price_scale,adjustment,source) VALUES(" +
				Quote(bar.m_instrument.m_strCode) + "," + std::to_string(static_cast<int>(bar.m_instrument.m_market)) + "," +
				std::to_string(static_cast<int>(bar.m_channel)) + "," + std::to_string(bar.m_nBeginTime) + "," +
				std::to_string(bar.m_nOpenPrice) + "," + std::to_string(bar.m_nHighPrice) + "," +
				std::to_string(bar.m_nLowPrice) + "," + std::to_string(bar.m_nClosePrice) + "," +
				std::to_string(bar.m_nVolume) + "," + std::to_string(bar.m_nTurnover) + "," +
				std::to_string(bar.m_nPriceScale) + "," + Quote(bar.m_strAdjustment) + "," + Quote(bar.m_strSource) +
				") ON CONFLICT(symbol,exchange,channel,begin_time,adjustment) DO UPDATE SET open=excluded.open,high=excluded.high,low=excluded.low,close=excluded.close,volume=excluded.volume,turnover=excluded.turnover,price_scale=excluded.price_scale,source=excluded.source";
			if (!Exec(sql))
			{
				bOk = false;
				break;
			}
		}

		if (bOk && m_db->EndTransaction())
		{
			return true;
		}
		m_db->RollBackTransaction();
		return false;
	}

	std::vector<market::CBar> CHQRecorder::QueryBars(const market::CSecurity& security, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime)
	{
		std::string sql = "SELECT begin_time,open,high,low,close,volume,turnover,price_scale,adjustment,source FROM bar WHERE symbol=" +
			Quote(security.m_strCode) + " AND exchange=" + std::to_string(static_cast<int>(security.m_market)) +
			" AND channel=" + std::to_string(static_cast<int>(channel)) + " AND begin_time BETWEEN " +
			std::to_string(nBeginTime) + " AND " + std::to_string(nEndTime) + " ORDER BY begin_time";


		std::vector<market::CBar> result;
		std::unique_lock<std::shared_mutex> lck(m_mtx_db);
		if (!m_bOpen || (nullptr == m_db))
		{
			return result;
		}
		const db::_TyRows& rows = m_db->ExecQuery(sql).second;
		result.reserve(rows.size());
		for (const auto& row : rows)
		{
			if (10 != row.size())
			{
				continue;
			}

			result.emplace_back();
			market::CBar& bar = result.back();
			bar.m_instrument = instrument;
			bar.m_channel = channel;
			if (!utility::to_number(row[0], bar.m_nBeginTime) || !utility::to_number(row[1], bar.m_nOpenPrice) ||
				!utility::to_number(row[2], bar.m_nHighPrice) || !utility::to_number(row[3], bar.m_nLowPrice) ||
				!utility::to_number(row[4], bar.m_nClosePrice) || !utility::to_number(row[5], bar.m_nVolume) ||
				!utility::to_number(row[6], bar.m_nTurnover) || !utility::to_number(row[7], bar.m_nPriceScale))
			{
				continue;
			}
			bar.m_strAdjustment = row[8];
			bar.m_strSource = row[9];
			
		}
		return result;
	}
