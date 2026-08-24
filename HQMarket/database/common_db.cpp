#include "common_db.h"
#include <mysql/field_types.h>
#include <unordered_map>
#include "../common/utility.h"

namespace db
{
	CConnectParam::CConnectParam(const std::string& strHost, unsigned int nPort, const std::string& strAccount,
		const std::string& strPasswd, const std::string& strDB, const std::string& strCharset)
		: m_strHost(strHost), m_nPort(nPort), m_strAccount(strAccount), m_strPasswd(strPasswd), m_strDataBase(strDB),
		  m_strCharset(strCharset)
	{
	}

	CConnectParam::CConnectParam(const std::string& strParam, char delimiter)
	{
		std::vector<std::string> data;
		if (utility::stringsplit(strParam, data, delimiter, false) == 6)
		{
			m_strHost = data[0];
			utility::s2n(data[1], m_nPort);
			m_strAccount = data[2];
			m_strPasswd = data[3];
			m_strDataBase = data[4];
			m_strCharset = data[5];
		}
	}

	std::string GetDBName(em_database ty)
	{
		switch (ty)
		{
		case em_database::mysql: return "mysql";
		case em_database::oracle: return "oracle";
		case em_database::sqlite: return "sqlite";
		default: return "";
		}
	}

	em_database GetDBType(const std::string& strName)
	{
		if (strName == "mysql") return em_database::mysql;
		if (strName == "oracle") return em_database::oracle;
		if (strName == "sqlite") return em_database::sqlite;
		return em_database::unknown;
	}

	DataTypes GetDataType(em_database ty, int nType)
	{
		if (ty != em_database::mysql) return DataTypes::em_string;
		switch (nType)
		{
		case MYSQL_TYPE_TINY:
		case MYSQL_TYPE_SHORT:
		case MYSQL_TYPE_INT24:
		case MYSQL_TYPE_ENUM: return DataTypes::em_int32;
		case MYSQL_TYPE_LONG:
		case MYSQL_TYPE_LONGLONG: return DataTypes::em_int64;
		case MYSQL_TYPE_DECIMAL:
		case MYSQL_TYPE_FLOAT:
		case MYSQL_TYPE_DOUBLE: return DataTypes::em_double;
		case MYSQL_TYPE_BOOL: return DataTypes::em_bool;
		case MYSQL_TYPE_TINY_BLOB:
		case MYSQL_TYPE_MEDIUM_BLOB:
		case MYSQL_TYPE_LONG_BLOB:
		case MYSQL_TYPE_BLOB: return DataTypes::binary;
		default: return DataTypes::em_string;
		}
	}
} // namespace db
