#include "CHQBroker.h"
#include "../log/CLogger.h"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <unordered_set>

namespace
{
	bool VerifyReferenceDirectory(const std::filesystem::path& path, std::int64_t finished, bool hongKong, CHQRecorder::CMarketSnapshot& snapshot)
	{
		std::chrono::sys_days day{ std::chrono::days{ market::MarketDateIndex(finished) } };
		std::chrono::year_month_day date{ day };
		int expectedDate = static_cast<int>(date.year()) * 10000 + static_cast<int>(static_cast<unsigned>(date.month())) * 100 + static_cast<int>(static_cast<unsigned>(date.day()));
		std::ifstream input(path);
		int referenceDate = 0;
		if (!(input >> referenceDate) || expectedDate != referenceDate)
		{
			return false;
		}
		std::unordered_map<std::string, market::InstrumentKind> reference;
		std::set<std::string> halted;
		std::unordered_map<std::string, std::int64_t> referencePrices;
		std::string line;
		std::getline(input, line);
		if (std::string::npos != line.find_first_not_of(" \t\r"))
		{
			return false;
		}
		while (std::getline(input, line))
		{
			std::istringstream row(line);
			std::string security;
			std::string kind;
			std::string extra;
			if (!(row >> security))
			{
				continue;
			}
			market::InstrumentKind classification = market::InstrumentKind::unknown;
			if (hongKong)
			{
				if (!(row >> kind) || ("stock" != kind && "other" != kind))
				{
					return false;
				}
				classification = "stock" == kind ? market::InstrumentKind::hkStock : market::InstrumentKind::hkOther;
			}
			while (row >> extra)
			{
				if ("halted" == extra && halted.emplace(security).second)
				{
					continue;
				}
				std::string_view attribute(extra);
				if (!attribute.starts_with("pre_close="))
				{
					return false;
				}
				attribute.remove_prefix(std::string_view("pre_close=").size());
				std::int64_t price = 0;
				if ("unknown" != attribute)
				{
					const char* end = attribute.data() + attribute.size();
					std::from_chars_result parsed = std::from_chars(attribute.data(), end, price);
					if (std::errc{} != parsed.ec || end != parsed.ptr || 0 >= price)
					{
						return false;
					}
				}
				if (!referencePrices.emplace(security, price).second)
				{
					return false;
				}
			}
			if (!reference.emplace(security, classification).second)
			{
				return false;
			}
		}
		std::set<std::string> actual;
		for (const auto& instrument : snapshot.m_instruments)
		{
			actual.emplace(instrument.m_security.String());
		}
		if (!input.eof() || reference.empty() || reference.size() != actual.size())
		{
			return false;
		}
		for (const auto& security : actual)
		{
			if (!reference.contains(security))
			{
				return false;
			}
		}
		for (auto& instrument : snapshot.m_instruments)
		{
			std::string security = instrument.m_security.String();
			if (hongKong)
			{
				instrument.m_kind = reference.at(security);
			}
			instrument.m_status = halted.contains(security) ? "halted" : "normal";
		}
		for (auto& quote : snapshot.m_quotes)
		{
			const auto mIter = referencePrices.find(quote.m_security.String());
			if (referencePrices.end() != mIter)
			{
				quote.m_pre_close = 4 == quote.m_price_scale ? mIter->second : 0;
			}
		}
		return true;
	}
} // namespace

CHQBroker::~CHQBroker()
{
	Stop();
}

std::string CHQBroker::MarketMetrics() const
{
	std::ostringstream out;
	out << m_mootdx.Metrics();
	std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch())
						   .count();
	std::shared_lock<std::shared_mutex> lock(m_snapshot_mutex);
	for (std::size_t market = 0; market < m_snapshots.size(); ++market)
	{
		std::string label = 0 == market ? "{market=\"CN\"}" : "{market=\"HK\"}";
		const std::optional<_TyMarketSnapshot>& snapshot = m_snapshots[market];
		out << "hqmarket_snapshot_available" << label << " " << snapshot.has_value() << "\n";
		if (!snapshot.has_value())
		{
			continue;
		}
		out << "hqmarket_snapshot_expected" << label << " " << snapshot->m_expected << "\n"
			<< "hqmarket_snapshot_received" << label << " " << snapshot->m_quotes.size() << "\n"
			<< "hqmarket_snapshot_directory_verified" << label << " " << snapshot->m_directory_verified << "\n"
			<< "hqmarket_snapshot_duration_ms" << label << " " << snapshot->m_finished_at - snapshot->m_started_at << "\n"
			<< "hqmarket_snapshot_age_ms" << label << " " << (std::max)(std::int64_t{ 0 }, now - snapshot->m_finished_at) << "\n";
	}
	return out.str();
}

bool CHQBroker::Initialize(const std::filesystem::path& root)
{
	if (!m_recorder.Open(root / "data" / "hqmarket.db"))
	{
		return false;
	}
	m_mootdx.SetQuoteHandler([this](market::CQuote&& quote)
	{
		market::CSecurity security = quote.m_security;
		std::uint64_t sequence = m_cache.Update(std::move(quote));
		if (0 == sequence)
		{
			return;
		}
		std::optional<market::CQuote> cached = m_cache.GetQuote(security, &sequence);
		if (cached.has_value() && m_quote_handler)
		{
			m_quote_handler(*cached, sequence);
		}
	});
	m_mootdx.SetDepthHandler([this](market::CDepth&& depth)
	{
		if (m_depth_handler)
		{
			m_depth_handler(std::move(depth));
		}
	});
	if (!m_mootdx.Initialize())
	{
		return false;
	}
	m_akshare.Initialize();
	const char* scanEnabled = std::getenv("HQMARKET_ENABLE_MARKET_SCAN");
	if (nullptr != scanEnabled && "1" == std::string_view(scanEnabled))
	{
		{
			std::unique_lock<std::shared_mutex> lock(m_snapshot_mutex);
			m_snapshots[0] = m_recorder.LoadMarketSnapshot();
			m_snapshots[1] = m_recorder.LoadMarketSnapshot(true);
			m_complete_snapshots[0] = m_recorder.LoadMarketSnapshot(false, true);
			m_complete_snapshots[1] = m_recorder.LoadMarketSnapshot(true, true);
		}
		ConfigureMarketScan(root);
		m_directory_thread = std::jthread([this, root](std::stop_token stop)
		{
			std::int64_t refreshed = 0;
			std::mutex mutex;
			std::condition_variable_any condition;
			std::unique_lock<std::mutex> lock(mutex);
			while (!stop.stop_requested())
			{
				condition.wait_for(lock, stop, std::chrono::minutes(5), []()
				{
					return false;
				});
				if (stop.stop_requested())
				{
					break;
				}
				std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::system_clock::now().time_since_epoch())
									   .count();
				std::int64_t day = market::MarketDateIndex(now);
				if (refreshed != day && m_akshare.RefreshInstruments())
				{
					ConfigureMarketScan(root);
					refreshed = day;
				}
			}
		});
	}
	return true;
}

void CHQBroker::ConfigureMarketScan(const std::filesystem::path& root)
{
	std::vector<market::CInstrument> instruments = m_akshare.QueryInstruments();
	std::set<std::pair<market::Exchange, market::InstrumentKind>> availableCategories;
	for (const auto& instrument : instruments)
	{
		availableCategories.emplace(instrument.m_security.m_market, instrument.m_kind);
	}
	std::shared_lock<std::shared_mutex> lock(m_snapshot_mutex);
	const auto& snapshots = m_snapshots;
	for (const auto& restored : snapshots)
	{
		if (!restored.has_value())
		{
			continue;
		}
		for (const auto& previous : restored->m_instruments)
		{
			market::InstrumentKind kind = market::Exchange::hkex == previous.m_security.m_market ? market::InstrumentKind::unknown : previous.m_kind;
			if (!availableCategories.contains({ previous.m_security.m_market, kind }))
			{
				instruments.push_back(previous);
				instruments.back().m_status = "normal";
				instruments.back().m_kind = kind;
			}
		}
	}
	lock.unlock();
	std::vector<market::CSecurity> securities;
	securities.reserve(instruments.size());
	for (const auto& instrument : instruments)
	{
		securities.push_back(instrument.m_security);
	}
	std::ranges::sort(securities, [](const market::CSecurity& left, const market::CSecurity& right)
	{
		return left.m_market != right.m_market ? left.m_market < right.m_market : left.m_code < right.m_code;
	});
	securities.erase(std::unique(securities.begin(), securities.end()), securities.end());
	m_mootdx.ConfigureScan(std::move(securities), root / "ini" / "market_sessions.txt", [this, instruments = std::move(instruments), referenceRoot = root / "ini"](bool hongKong, std::size_t expected, std::int64_t started, std::int64_t finished, std::vector<market::CQuote>&& quotes)
	{
		if (quotes.empty() || market::MarketDateIndex(started) != market::MarketDateIndex(finished))
		{
			return;
		}
		for (const auto& quote : quotes)
		{
			m_cache.Update(quote);
		}
		std::uint64_t version = static_cast<std::uint64_t>(finished);
		{
			std::shared_lock<std::shared_mutex> lock(m_snapshot_mutex);
			if (m_snapshots[hongKong].has_value())
			{
				version = (std::max)(version, m_snapshots[hongKong]->m_version + 1);
			}
		}
		std::vector<market::CInstrument> directory;
		std::ranges::copy_if(instruments, std::back_inserter(directory), [hongKong](const market::CInstrument& instrument)
		{
			return hongKong == (market::Exchange::hkex == instrument.m_security.m_market);
		});
		_TyMarketSnapshot snapshot{ version, expected, started, finished, std::move(quotes), std::move(directory) };
		snapshot.m_directory_verified = VerifyReferenceDirectory(referenceRoot / (hongKong ? "market_reference_hk.txt" : "market_reference_cn.txt"), finished, hongKong, snapshot);
		if (!m_recorder.SaveMarketSnapshot(snapshot, hongKong))
		{
			CLogger::Write(Level::Error, "market", "market snapshot persistence failed");
			return;
		}
		std::unordered_set<market::CSecurity, market::CSecurityHash> covered;
		for (const auto& quote : snapshot.m_quotes)
		{
			covered.emplace(quote.m_security);
		}
		for (const auto& instrument : snapshot.m_instruments)
		{
			if ("halted" == instrument.m_status)
			{
				covered.emplace(instrument.m_security);
			}
		}
		bool complete = snapshot.m_directory_verified && covered.size() == snapshot.m_expected;
		bool completeSaved = complete && m_recorder.SaveMarketSnapshot(snapshot, hongKong, true);
		if (complete && !completeSaved)
		{
			CLogger::Write(Level::Error, "market", "complete market snapshot persistence failed");
		}
		std::unique_lock<std::shared_mutex> lock(m_snapshot_mutex);
		if (completeSaved)
		{
			m_complete_snapshots[hongKong] = snapshot;
		}
		m_previous_snapshots[hongKong] = std::move(m_snapshots[hongKong]);
		m_snapshots[hongKong].emplace(std::move(snapshot));
	});
}

std::optional<CHQBroker::_TyMarketSnapshot> CHQBroker::QueryMarketSnapshot(std::uint64_t version, bool hongKong, bool complete) const
{
	std::shared_lock<std::shared_mutex> lock(m_snapshot_mutex);
	if (m_complete_snapshots[hongKong].has_value() && ((0 == version && complete) || m_complete_snapshots[hongKong]->m_version == version))
	{
		return m_complete_snapshots[hongKong];
	}
	if (m_snapshots[hongKong].has_value() && (0 == version || m_snapshots[hongKong]->m_version == version))
	{
		return m_snapshots[hongKong];
	}
	if (m_previous_snapshots[hongKong].has_value() && m_previous_snapshots[hongKong]->m_version == version)
	{
		return m_previous_snapshots[hongKong];
	}
	return std::nullopt;
}

void CHQBroker::Stop()
{
	if (m_directory_thread.joinable())
	{
		m_directory_thread.request_stop();
		m_directory_thread.join();
	}
	m_mootdx.Stop();
	std::lock_guard<std::mutex> fetchLock(m_mtx_historyfetch);
	m_akshare.Stop();
	std::lock_guard<std::mutex> lock(m_mtx_history);
	m_historyRefreshes.clear();
	m_recorder.Close();
}

bool CHQBroker::Subscribe(const std::vector<market::CChannelInfo>& infos)
{
	return m_mootdx.Subscribe(infos);
}

bool CHQBroker::Unsubscribe(const std::vector<market::CChannelInfo>& infos)
{
	return m_mootdx.Unsubscribe(infos);
}

std::optional<market::CQuote> CHQBroker::QueryQuote(const market::CSecurity& security, std::uint64_t* sequence) const
{
	return m_cache.GetQuote(security, sequence);
}

std::vector<market::CBar> CHQBroker::QueryBars(const market::CSecurity& security, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime, market::CProviderStatus* queryStatus)
{

	std::string key = security.String() + ":" + std::to_string(static_cast<int>(channel)) + ":" +
					  std::to_string(nBeginTime) + ":" + std::to_string(nEndTime);
	if (0 == nEndTime)
	{
		nEndTime = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch())
					   .count();
	}
	auto isFresh = [this, &key]()
	{
		std::lock_guard<std::mutex> lock(m_mtx_history);
		auto refreshed = m_historyRefreshes.find(key);
		return m_historyRefreshes.end() != refreshed && std::chrono::steady_clock::now() < refreshed->second;
	};
	if (isFresh())
	{
		return m_recorder.QueryBars(security, channel, nBeginTime, nEndTime);
	}
	std::lock_guard<std::mutex> fetchLock(m_mtx_historyfetch);
	std::vector<market::CBar> bars = m_recorder.QueryBars(security, channel, nBeginTime, nEndTime);
	if (isFresh())
	{
		return bars;
	}
	std::vector<market::CBar> providerBars = m_akshare.QueryBars(security, channel, nBeginTime, nEndTime, queryStatus);
	if (!providerBars.empty())
	{
		std::map<std::int64_t, market::CBar> mergedBars;
		for (const market::CBar& bar : bars)
		{
			mergedBars.insert_or_assign(bar.m_begin_time, bar);
		}
		for (market::CBar& bar : providerBars)
		{
			auto previous = mergedBars.find(bar.m_begin_time);
			if (!bar.m_hasTurnover && mergedBars.end() != previous)
			{
				bar.m_turnover = previous->second.m_turnover;
			}
			mergedBars.insert_or_assign(bar.m_begin_time, bar);
		}
		if (!m_recorder.UpsertBars(providerBars))
		{
			CLogger::Write(Level::Error, "history", "Failed to persist bars for ", security.String());
		}
		else
		{
			bool primary = true;
			for (const market::CBar& bar : providerBars)
			{
				primary = primary && ("akshare" == bar.m_source);
			}
			if (primary)
			{
				std::lock_guard<std::mutex> lock(m_mtx_history);
				if (1024 <= m_historyRefreshes.size())
				{
					m_historyRefreshes.clear();
				}
				std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::system_clock::now().time_since_epoch())
										 .count();
				auto ttl = nEndTime < nowMs - 86400000 ? std::chrono::seconds(300) : std::chrono::seconds(5);
				m_historyRefreshes.insert_or_assign(std::move(key), std::chrono::steady_clock::now() + ttl);
			}
		}
		bars.clear();
		bars.reserve(mergedBars.size());
		for (const auto& value : mergedBars)
		{
			bars.emplace_back(value.second);
		}
	}
	return bars;
}

std::vector<market::CInstrument> CHQBroker::QueryInstruments() const
{
	return m_akshare.QueryInstruments();
}

std::vector<market::CSector> CHQBroker::QuerySectors(market::SectorType type)
{
	return m_akshare.QuerySectors(type);
}

market::CSectorConstituents CHQBroker::QuerySectorConstituents(market::SectorType type, const std::string& strSectorCode)
{
	return m_akshare.QuerySectorConstituents(type, strSectorCode);
}

market::CProviderStatus CHQBroker::RealtimeStatus() const
{
	return m_mootdx.GetStatus();
}

market::CProviderStatus CHQBroker::HistoryStatus() const
{
	return m_akshare.GetStatus();
}

bool CHQBroker::IsRecorderOpen() const
{
	return m_recorder.IsOpen();
}

std::size_t CHQBroker::QuoteCount() const
{
	return m_cache.QuoteCount();
}

void CHQBroker::SetQuoteHandler(_TyQuoteHandler handler)
{
	m_quote_handler = std::move(handler);
}

void CHQBroker::SetDepthHandler(_TyDepthHandler handler)
{
	m_depth_handler = std::move(handler);
}
