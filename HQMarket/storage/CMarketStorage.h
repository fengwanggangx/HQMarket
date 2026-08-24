#ifndef __CMARKET_STORAGE_H__
#define __CMARKET_STORAGE_H__
#include "../market/MarketTypes.h"
#include "../database/CSQLite3.h"
#include <filesystem>
#include <mutex>
#include <vector>
namespace storage
{
	class CMarketStorage final
	{
		public:
			~CMarketStorage();
			bool Open(const std::filesystem::path& path);
			void Close();
			bool UpsertBars(const std::vector<market::CBar>& bars);
			std::vector<market::CBar> QueryBars(const market::CInstrument& instrument, market::Channel channel,
												std::int64_t nBeginTime, std::int64_t nEndTime);
			bool IsOpen() const;

		private:
			bool Exec(const std::string& strSql);
			mutable std::mutex m_mtx_database;
			db::CSQLite3 m_database;
			bool m_bOpen{false};
	};
} // namespace storage
#endif
