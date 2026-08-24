#pragma once

#include "../database/CDBEngine.h"
#include "../market/MarketTypes.h"
#include <filesystem>
#include <mutex>
#include <vector>

class CHQRecorder final
{
public:
	CHQRecorder() = default;
	~CHQRecorder();
	bool Open(const std::filesystem::path& path);
	void Close();
	bool UpsertBars(const std::vector<market::CBar>& bars);
	std::vector<market::CBar> QueryBars(const market::CInstrument& instrument, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime);
	bool IsOpen() const;

private:
	bool Exec(const std::string& strSQL);

private:
	mutable std::shared_mutex m_mtx_db;
	db::_TyDBPtr m_db;
	bool m_bOpen{ false };
};
