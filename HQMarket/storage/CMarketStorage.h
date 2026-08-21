#ifndef __CMARKET_STORAGE_H__
#define __CMARKET_STORAGE_H__
#include "../market/MarketTypes.h"
#include <filesystem>
#include <mutex>
#include <vector>
struct sqlite3;
namespace storage
{
	class CMarketStorage final
	{
	public:
		~CMarketStorage();
		bool Open(const std::filesystem::path& path);
		void Close();
		bool UpsertBars(const std::vector<market::CBar>& bars);
		std::vector<market::CBar> QueryBars(const market::CInstrument& instrument, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime);
		bool IsOpen() const;
	private:
		bool Exec(const char* pszSql);
		mutable std::mutex m_mtxDatabase;
		sqlite3* m_pDatabase{ nullptr };
	};
}
#endif
