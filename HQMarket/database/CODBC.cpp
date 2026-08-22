#include "CODBC.h"
#include <unordered_map>
#include "CSQLite3.h"
#include "CMySQL.h"
#include "COracle.h"

namespace db
{
	std::unique_ptr<IDataBase> CreateDB(db::database ty)
	{
		switch (ty)
		{
		case db::database::sqlite:
			return std::make_unique<CSQLite3>();
		case db::database::mysql:
			return std::make_unique<CMySQL>();
		case db::database::oracle:
			return std::make_unique<COracle>();
		default:
			break;
		}
		return nullptr;
	}

	CODBC::CODBC()
	{
		m_Releasor = [this](IDataBase* pDB)
		{
			if (pDB != nullptr)
			{
				std::lock_guard<std::mutex> lck(m_mtx);
				pDB->m_status = status::free;
			}
		};
	}

	CODBC::~CODBC()
	{
		Close();
	}

	int CODBC::Connect(db::database ty, const CConnectParam& param, int nCount)
	{
		std::lock_guard<std::mutex> lck(m_mtx);
		_TyPool& pool = m_database[ty];
		for (int i = 0; i < nCount; ++i)
		{
			std::unique_ptr<IDataBase> pDB = CreateDB(ty);
			if (pDB == nullptr)
			{
				return -1;
			}
			pDB->Connect(param);
			pool.emplace_back(std::move(pDB));
		}
		return 0;
	}

	int CODBC::Close()
	{
		std::lock_guard<std::mutex> lck(m_mtx);
		for (const auto& data : m_database)
		{
			for (const _TyOwnedDB& item : data.second)
			{
				if (item != nullptr)
				{
					item->Close();
				}
			}
		}
		m_database.clear();
		return 0;
	}

	int CODBC::Close(db::database ty)
	{
		std::lock_guard<std::mutex> lck(m_mtx);
		std::unordered_map<db::database, _TyPool>::iterator mIter = m_database.find(ty);
		if (m_database.end() == mIter)
		{
			return 0;
		}
		for (const _TyOwnedDB& item : mIter->second)
		{
			if (item != nullptr)
			{
				item->Close();
			}
		}
		m_database.erase(ty);
		return 0;
	}

	_TyDBPtr CODBC::GetADataBase(db::database ty)
	{
		std::lock_guard<std::mutex> lck(m_mtx);
		std::unordered_map<db::database, _TyPool>::iterator mIter = m_database.find(ty);
		if (m_database.end() == mIter)
		{
			return nullptr;
		}

		_TyPool& pool = mIter->second;
		for (const _TyOwnedDB& item : pool)
		{
			if (item == nullptr)
			{
				continue;
			}
			if (item->m_status != db::status::free)
			{
				continue;
			}

			item->m_status = db::status::busy;
			return _TyDBPtr(item.get(), m_Releasor);
		}
		return nullptr;
	}

} // namespace db
