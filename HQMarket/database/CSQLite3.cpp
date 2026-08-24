#include "CSQLite3.h"
#include <sqlite3.h>

namespace db
{
	CSQLite3::CSQLite3()
	{
	}

	CSQLite3::~CSQLite3()
	{
		Close();
	}

	int CSQLite3::Connect(const CConnectParam& param)
	{
		if (m_pDB != nullptr) return 0;
		sqlite3* pDatabase = nullptr;
		int result = sqlite3_open_v2(param.m_strDataBase.c_str(), &pDatabase, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
		if (result != SQLITE_OK)
		{
			if (pDatabase != nullptr) sqlite3_close(pDatabase);
			return result;
		}
		m_pDB = pDatabase;
		return SQLITE_OK;
	}

	int CSQLite3::Close()
	{
		if (m_pDB == nullptr) return SQLITE_OK;
		int result = sqlite3_close(static_cast<sqlite3*>(m_pDB));
		if (result == SQLITE_OK) m_pDB = nullptr;
		return result;
	}

	int CSQLite3::ExecUpdate(const std::string& strSQL)
	{
		if (m_pDB == nullptr) return SQLITE_MISUSE;
		return sqlite3_exec(static_cast<sqlite3*>(m_pDB), strSQL.c_str(), nullptr, nullptr, nullptr);
	}

	const _TyTableInfo& CSQLite3::ExecQuery(const std::string& strSQL)
	{
		m_table = {};
		if (m_pDB == nullptr) return m_table;
		sqlite3_stmt* pStatement = nullptr;
		if (sqlite3_prepare_v2(static_cast<sqlite3*>(m_pDB), strSQL.c_str(), -1, &pStatement, nullptr) != SQLITE_OK) return m_table;
		const int nColumns = sqlite3_column_count(pStatement);
		m_table.first.reserve(nColumns);
		for (int i = 0; i < nColumns; ++i)
		{
			CColumnInfo column;
			column.m_uId = static_cast<unsigned int>(i);
			const char* pszName = sqlite3_column_name(pStatement, i);
			column.m_strName = pszName == nullptr ? "" : pszName;
			m_table.first.emplace_back(std::move(column));
		}
		while (sqlite3_step(pStatement) == SQLITE_ROW)
		{
			std::vector<std::string> row;
			row.reserve(nColumns);
			for (int i = 0; i < nColumns; ++i)
			{
				const unsigned char* pszValue = sqlite3_column_text(pStatement, i);
				row.emplace_back(pszValue == nullptr ? "" : reinterpret_cast<const char*>(pszValue));
			}
			m_table.second.emplace_back(std::move(row));
		}
		sqlite3_finalize(pStatement);
		return m_table;
	}

	bool CSQLite3::BeginTransaction()
	{
		return ExecUpdate("BEGIN IMMEDIATE") == SQLITE_OK;
	}

	bool CSQLite3::EndTransaction()
	{
		return ExecUpdate("COMMIT") == SQLITE_OK;
	}

	bool CSQLite3::RollBackTransaction()
	{
		return ExecUpdate("ROLLBACK") == SQLITE_OK;
	}

} // namespace db
