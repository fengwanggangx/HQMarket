#include "CHQRecorder.h"
#include "../common/utility.h"
#include "../database/IDataBase.h"
#include <charconv>
#include <system_error>
#include <unordered_set>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

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
} // namespace

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

	std::unique_lock<std::shared_mutex> lck(m_db_mutex);
	if (m_open)
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

	m_open = Exec("PRAGMA journal_mode=WAL;") &&
			 Exec("PRAGMA synchronous=NORMAL;") &&
			 Exec("CREATE TABLE IF NOT EXISTS instrument(symbol TEXT NOT NULL, exchange INTEGER NOT NULL, name TEXT NOT NULL DEFAULT '', updated_at INTEGER NOT NULL, PRIMARY KEY(symbol, exchange));") &&
			 Exec("CREATE TABLE IF NOT EXISTS bar(symbol TEXT NOT NULL, exchange INTEGER NOT NULL, channel INTEGER NOT NULL, begin_time INTEGER NOT NULL, open INTEGER NOT NULL, high INTEGER NOT NULL, low INTEGER NOT NULL, close INTEGER NOT NULL, volume INTEGER NOT NULL, turnover INTEGER NOT NULL, price_scale INTEGER NOT NULL, adjustment TEXT NOT NULL, source TEXT NOT NULL, PRIMARY KEY(symbol, exchange, channel, begin_time, adjustment));") &&
			 Exec("CREATE TABLE IF NOT EXISTS sync_state(dataset TEXT PRIMARY KEY, cursor TEXT NOT NULL, updated_at INTEGER NOT NULL);");

	if (m_open)
	{
		std::unordered_set<std::string> columns;
		db::CQueryTable schema = m_db->ExecQuery("PRAGMA table_info(bar);");
		for (const auto& row : schema.m_rows)
		{
			if (1 < row.size())
			{
				columns.insert(db::QueryValueToString(row[1]));
			}
		}
		m_open = m_db->BeginTransaction();
		if (m_open)
		{
			m_open = (columns.contains("end_time") || Exec("ALTER TABLE bar ADD COLUMN end_time INTEGER NOT NULL DEFAULT 0;")) &&
					 (columns.contains("completed") || Exec("ALTER TABLE bar ADD COLUMN completed INTEGER NOT NULL DEFAULT 0;")) &&
					 (columns.contains("data_version") || Exec("ALTER TABLE bar ADD COLUMN data_version TEXT NOT NULL DEFAULT '0';")) &&
					 (columns.contains("basis_version") || Exec("ALTER TABLE bar ADD COLUMN basis_version TEXT NOT NULL DEFAULT '';")) &&
					 m_db->EndTransaction();
			if (!m_open)
			{
				m_db->RollBackTransaction();
			}
		}
	}
	if (!m_open)
	{
		m_db.reset();
	}
	return m_open;
}

void CHQRecorder::Close()
{
	std::unique_lock<std::shared_mutex> lck(m_db_mutex);
	m_db.reset();
	m_open = false;
}

bool CHQRecorder::IsOpen() const
{
	std::shared_lock<std::shared_mutex> lck(m_db_mutex);
	return m_open && (nullptr != m_db);
}

bool CHQRecorder::SaveMarketSnapshot(const CMarketSnapshot& snapshot, bool hongKong, bool complete)
{
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartArray();
	writer.Uint64(snapshot.m_version);
	writer.Uint64(snapshot.m_expected);
	writer.Int64(snapshot.m_started_at);
	writer.Int64(snapshot.m_finished_at);
	writer.StartArray();
	for (const auto& instrument : snapshot.m_instruments)
	{
		writer.StartArray();
		writer.String(instrument.m_security.m_code.c_str());
		writer.Int(static_cast<int>(instrument.m_security.m_market));
		writer.Int(static_cast<int>(instrument.m_kind));
		writer.String(instrument.m_status.c_str());
		writer.EndArray();
	}
	writer.EndArray();
	writer.StartArray();
	for (const auto& quote : snapshot.m_quotes)
	{
		writer.StartArray();
		writer.String(quote.m_security.m_code.c_str());
		writer.Int(static_cast<int>(quote.m_security.m_market));
		writer.String(quote.m_source.c_str());
		for (std::int64_t value : { quote.m_exchange_time, quote.m_receive_time, quote.m_last_price, quote.m_open_price,
				 quote.m_high_price, quote.m_low_price, quote.m_pre_close, quote.m_volume, quote.m_turnover,
				 static_cast<std::int64_t>(quote.m_price_scale) })
		{
			writer.Int64(value);
		}
		writer.EndArray();
	}
	writer.EndArray();
	writer.Bool(snapshot.m_directory_verified);
	writer.EndArray();
	std::string key = hongKong ? "market_snapshot_hk_v2" : "market_snapshot_cn_v2";
	if (complete)
	{
		key += "_complete";
	}
	std::string sql = "INSERT OR REPLACE INTO sync_state(dataset,cursor,updated_at) VALUES(" + Quote(key) + "," +
					  Quote(buffer.GetString()) + "," + std::to_string(snapshot.m_finished_at) + ");";
	std::unique_lock<std::shared_mutex> lock(m_db_mutex);
	return m_open && Exec(sql);
}

std::optional<CHQRecorder::CMarketSnapshot> CHQRecorder::LoadMarketSnapshot(bool hongKong, bool complete)
{
	std::string payload;
	{
		std::unique_lock<std::shared_mutex> lock(m_db_mutex);
		if (!m_open)
		{
			return std::nullopt;
		}
		std::string key = hongKong ? "market_snapshot_hk_v2" : "market_snapshot_cn_v2";
		if (complete)
		{
			key += "_complete";
		}
		const db::CQueryTable::_TyRows& rows = m_db->ExecQuery("SELECT cursor FROM sync_state WHERE dataset=" + Quote(key) + ";").m_rows;
		if (rows.empty() || rows.front().empty())
		{
			return std::nullopt;
		}
		payload = db::QueryValueToString(rows.front().front());
	}
	rapidjson::Document data;
	data.Parse(payload.c_str());
	if (!data.IsArray() || (6 != data.Size() && 7 != data.Size()) || !data[0].IsUint64() || !data[1].IsUint64() ||
		!data[2].IsInt64() || !data[3].IsInt64() || !data[4].IsArray() || !data[5].IsArray())
	{
		return std::nullopt;
	}
	CMarketSnapshot snapshot;
	snapshot.m_version = data[0].GetUint64();
	snapshot.m_expected = data[1].GetUint64();
	snapshot.m_started_at = data[2].GetInt64();
	snapshot.m_finished_at = data[3].GetInt64();
	snapshot.m_directory_verified = 7 == data.Size() && data[6].IsTrue();
	for (const auto& row : data[4].GetArray())
	{
		if (!row.IsArray() || (3 != row.Size() && 4 != row.Size()) || !row[0].IsString() || !row[1].IsInt() || !row[2].IsInt() || (4 == row.Size() && !row[3].IsString()))
		{
			return std::nullopt;
		}
		market::CInstrument instrument;
		instrument.m_security = market::CSecurity(row[0].GetString(), static_cast<market::Exchange>(row[1].GetInt()));
		instrument.m_kind = static_cast<market::InstrumentKind>(row[2].GetInt());
		if (4 == row.Size())
		{
			instrument.m_status = row[3].GetString();
		}
		snapshot.m_instruments.emplace_back(std::move(instrument));
	}
	for (const auto& row : data[5].GetArray())
	{
		if (!row.IsArray() || 13 != row.Size() || !row[0].IsString() || !row[1].IsInt() || !row[2].IsString() || !row[12].IsInt())
		{
			return std::nullopt;
		}
		for (rapidjson::SizeType field = 3; field < 12; ++field)
		{
			if (!row[field].IsInt64())
			{
				return std::nullopt;
			}
		}
		market::CQuote quote;
		quote.m_security = market::CSecurity(row[0].GetString(), static_cast<market::Exchange>(row[1].GetInt()));
		quote.m_source = row[2].GetString();
		quote.m_exchange_time = row[3].GetInt64();
		quote.m_receive_time = row[4].GetInt64();
		quote.m_last_price = row[5].GetInt64();
		quote.m_open_price = row[6].GetInt64();
		quote.m_high_price = row[7].GetInt64();
		quote.m_low_price = row[8].GetInt64();
		quote.m_pre_close = row[9].GetInt64();
		quote.m_volume = row[10].GetInt64();
		quote.m_turnover = row[11].GetInt64();
		quote.m_price_scale = row[12].GetInt();
		snapshot.m_quotes.emplace_back(std::move(quote));
	}
	return snapshot;
}

bool CHQRecorder::UpsertBars(const std::vector<market::CBar>& bars)
{
	std::unique_lock<std::shared_mutex> lck(m_db_mutex);
	if (!m_open || (nullptr == m_db))
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
		std::string sql = "INSERT INTO bar(symbol,exchange,channel,begin_time,open,high,low,close,volume,turnover,price_scale,adjustment,source,end_time,completed,data_version,basis_version) VALUES(" +
						  Quote(bar.m_security.m_code) + "," + std::to_string(static_cast<int>(bar.m_security.m_market)) + "," +
						  std::to_string(static_cast<int>(bar.m_channel)) + "," + std::to_string(bar.m_begin_time) + "," +
						  std::to_string(bar.m_open_price) + "," + std::to_string(bar.m_high_price) + "," +
						  std::to_string(bar.m_low_price) + "," + std::to_string(bar.m_close_price) + "," +
						  std::to_string(bar.m_volume) + "," + std::to_string(bar.m_turnover) + "," +
						  std::to_string(bar.m_price_scale) + "," + Quote(bar.m_adjustment) + "," + Quote(bar.m_source) + "," +
						  std::to_string(bar.m_end_time) + "," + std::to_string(bar.m_completed) + "," +
						  Quote(std::to_string(bar.m_data_version)) + "," + Quote(bar.m_basis_version) +
						  ") ON CONFLICT(symbol,exchange,channel,begin_time,adjustment) DO UPDATE SET open=excluded.open,high=excluded.high,low=excluded.low,close=excluded.close,volume=excluded.volume,turnover=" +
						  (bar.m_hasTurnover ? "excluded.turnover" : "bar.turnover") + ",price_scale=excluded.price_scale,source=excluded.source,end_time=excluded.end_time,completed=excluded.completed,data_version=excluded.data_version,basis_version=excluded.basis_version";
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
	std::string sql = "SELECT begin_time,open,high,low,close,volume,turnover,price_scale,adjustment,source,end_time,completed,data_version,basis_version FROM bar WHERE symbol=" +
					  Quote(security.m_code) + " AND exchange=" + std::to_string(static_cast<int>(security.m_market)) +
					  " AND channel=" + std::to_string(static_cast<int>(channel)) + " AND adjustment IN ('','none') AND begin_time BETWEEN " +
					  std::to_string(nBeginTime) + " AND " + std::to_string(nEndTime);
	if (market::Channel::bar_1m == channel)
	{
		sql += " AND source<>'tencent'";
	}
	sql += " ORDER BY begin_time, adjustment DESC";

	std::vector<market::CBar> result;
	std::unique_lock<std::shared_mutex> lck(m_db_mutex);
	if (!m_open || (nullptr == m_db))
	{
		return result;
	}
	const db::CQueryTable::_TyRows& rows = m_db->ExecQuery(sql).m_rows;
	result.reserve(rows.size());
	for (const auto& row : rows)
	{
		if (14 != row.size())
		{
			continue;
		}

		market::CBar bar;
		bar.m_security = security;
		bar.m_channel = channel;
		if (!utility::to_number(db::QueryValueToString(row[0]), bar.m_begin_time) || !utility::to_number(db::QueryValueToString(row[1]), bar.m_open_price) ||
			!utility::to_number(db::QueryValueToString(row[2]), bar.m_high_price) || !utility::to_number(db::QueryValueToString(row[3]), bar.m_low_price) ||
			!utility::to_number(db::QueryValueToString(row[4]), bar.m_close_price) || !utility::to_number(db::QueryValueToString(row[5]), bar.m_volume) ||
			!utility::to_number(db::QueryValueToString(row[6]), bar.m_turnover) || !utility::to_number(db::QueryValueToString(row[7]), bar.m_price_scale))
		{
			continue;
		}
		int completed = 0;
		if (!utility::to_number(db::QueryValueToString(row[10]), bar.m_end_time) ||
			!utility::to_number(db::QueryValueToString(row[11]), completed) ||
			!utility::to_number(db::QueryValueToString(row[12]), bar.m_data_version))
		{
			continue;
		}
		bar.m_completed = 0 != completed;
		bar.m_basis_version = db::QueryValueToString(row[13]);
		bar.m_adjustment = db::QueryValueToString(row[8]);
		bar.m_source = db::QueryValueToString(row[9]);
		if (result.empty() || result.back().m_begin_time != bar.m_begin_time)
		{
			result.emplace_back(std::move(bar));
		}
	}
	return result;
}
