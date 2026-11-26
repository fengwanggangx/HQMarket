#pragma once

#include "../database/CDBEngine.h"
#include "MarketTypes.h"
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

class CHQRecorder final
{
public:
	struct CMarketSnapshot
	{
		std::uint64_t m_version{ 0 };
		std::size_t m_expected{ 0 };
		std::int64_t m_started_at{ 0 };
		std::int64_t m_finished_at{ 0 };
		std::vector<market::CQuote> m_quotes;
		std::vector<market::CInstrument> m_instruments;
		bool m_directory_verified{ false };
	};

	CHQRecorder() = default;
	~CHQRecorder();
	bool Open(const std::filesystem::path& path);
	void Close();
	bool UpsertBars(const std::vector<market::CBar>& bars);
	std::vector<market::CBar> QueryBars(const market::CSecurity& security, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime);
	bool IsOpen() const;
	bool SaveMarketSnapshot(const CMarketSnapshot& snapshot, bool hongKong = false, bool complete = false);
	std::optional<CMarketSnapshot> LoadMarketSnapshot(bool hongKong = false, bool complete = false);

private:
	bool Exec(const std::string& strSQL);

private:
	mutable std::shared_mutex m_db_mutex;
	db::_TyDBPtr m_db;
	bool m_open{ false };
};
