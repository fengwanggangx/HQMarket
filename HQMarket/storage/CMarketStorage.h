#pragma once

#include "../database/CSQLite3.h"
#include "../market/MarketTypes.h"
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
			std::vector<market::CBar> QueryBars(const market::CInstrument& instrument, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime);
			bool IsOpen() const;

		private:
			bool Exec(const std::string& strSQL);
			mutable std::mutex m_mtx_database;
			db::CSQLite3 m_database;
			bool m_bOpen{false};
	};
} // namespace storage
