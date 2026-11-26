#ifndef __CHQ_BROKER_H__
#define __CHQ_BROKER_H__

#include "CHQCache.h"
#include "CHQRecorder.h"
#include "../provider/CAkShareProvider.h"
#include "../provider/CMooTdxProvider.h"
#include <array>
#include <chrono>
#include <thread>
#include <mutex>
#include <string>
#include <unordered_map>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

class CHQBroker final
{
public:
	using _TyQuoteHandler = std::function<void(const market::CQuote&, std::uint64_t)>;
	using _TyDepthHandler = std::function<void(market::CDepth&&)>;

	using _TyMarketSnapshot = CHQRecorder::CMarketSnapshot;

	~CHQBroker();
	bool Initialize(const std::filesystem::path& root);
	void Stop();
	bool Subscribe(const std::vector<market::CChannelInfo>& infos);
	bool Unsubscribe(const std::vector<market::CChannelInfo>& infos);
	std::optional<market::CQuote> QueryQuote(const market::CSecurity& security, std::uint64_t* sequence = nullptr) const;
	std::vector<market::CBar> QueryBars(const market::CSecurity& security, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime, market::CProviderStatus* queryStatus = nullptr);
	std::vector<market::CInstrument> QueryInstruments() const;
	std::optional<_TyMarketSnapshot> QueryMarketSnapshot(std::uint64_t version, bool hongKong, bool complete = false) const;
	std::vector<market::CSector> QuerySectors(market::SectorType type);
	market::CSectorConstituents QuerySectorConstituents(market::SectorType type, const std::string& strSectorCode);
	market::CProviderStatus RealtimeStatus() const;
	market::CProviderStatus HistoryStatus() const;
	bool IsRecorderOpen() const;
	std::size_t QuoteCount() const;
	std::string MarketMetrics() const;
	void SetQuoteHandler(_TyQuoteHandler handler);
	void SetDepthHandler(_TyDepthHandler handler);

private:
	void ConfigureMarketScan(const std::filesystem::path& root);

	std::jthread m_directory_thread;
	provider::CMooTdxProvider m_mootdx;
	provider::CAkShareProvider m_akshare;
	CHQCache m_cache;
	CHQRecorder m_recorder;
	mutable std::shared_mutex m_snapshot_mutex;
	std::array<std::optional<_TyMarketSnapshot>, 2> m_snapshots;
	std::array<std::optional<_TyMarketSnapshot>, 2> m_previous_snapshots;
	std::array<std::optional<_TyMarketSnapshot>, 2> m_complete_snapshots;
	std::mutex m_mtx_history;
	std::mutex m_mtx_historyfetch;
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_historyRefreshes;
	_TyQuoteHandler m_quote_handler;
	_TyDepthHandler m_depth_handler;
};

#endif
