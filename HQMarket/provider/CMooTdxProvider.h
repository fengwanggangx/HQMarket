#ifndef __CMOOTDX_PROVIDER_H__
#define __CMOOTDX_PROVIDER_H__
#include "../quote/IMarketProvider.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <array>
#include <algorithm>
#include <sstream>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
namespace provider
{
	class CMooTdxProvider final : public market::IMarketProvider
	{
	public:
		using _TyScanHandler = std::function<void(bool, std::size_t, std::int64_t, std::int64_t, std::vector<market::CQuote>&&)>;

		~CMooTdxProvider() override;
		const char* Name() const override;
		bool Initialize() override;
		bool Subscribe(const std::vector<market::CChannelInfo>& subscriptions) override;
		bool Unsubscribe(const std::vector<market::CChannelInfo>& subscriptions) override;
		std::vector<market::CBar> QueryBars(const market::CSecurity&, market::Channel, std::int64_t, std::int64_t) override;
		market::CProviderStatus GetStatus() const override;
		void SetQuoteHandler(_TyQuoteHandler handler) override;
		void SetDepthHandler(_TyDepthHandler handler) override;
		void Stop() override;
		std::string Metrics() const
		{
			std::vector<std::int64_t> samples;
			std::vector<std::int64_t> intervals;
			std::uint64_t failures = 0;
			{
				std::lock_guard<std::mutex> lock(m_state_mutex);
				samples.assign(m_request_durations.begin(), m_request_durations.begin() + m_duration_count);
				intervals.assign(m_refresh_intervals.begin(), m_refresh_intervals.begin() + m_interval_count);
				failures = m_subscription_failures;
			}
			std::ranges::sort(samples);
			std::ranges::sort(intervals);
			std::ostringstream out;
			out << "hqmarket_subscription_request_failures_total " << failures << "\n";
			for (int percentile : { 95, 99 })
			{
				out << "hqmarket_subscription_request_duration_ms{quantile=\"0." << percentile << "\"} ";
				if (samples.empty())
				{
					out << "NaN";
				}
				else
				{
					out << samples[(samples.size() * percentile + 99) / 100 - 1];
				}
				out << "\n";
				out << "hqmarket_subscription_refresh_interval_ms{quantile=\"0." << percentile << "\"} ";
				if (intervals.empty())
				{
					out << "NaN";
				}
				else
				{
					out << intervals[(intervals.size() * percentile + 99) / 100 - 1];
				}
				out << "\n";
			}
			return out.str();
		}
		void ConfigureScan(std::vector<market::CSecurity> securities, const std::filesystem::path& calendar, _TyScanHandler&& handler)
		{
			std::lock_guard<std::mutex> lock(m_state_mutex);
			m_scan_securities = std::move(securities);
			m_scan_calendar = calendar;
			m_scan_handler = std::move(handler);
			++m_scan_revision;
		}

	private:
		void Run();
		void Wait(std::chrono::milliseconds duration);
		std::size_t Poll(const std::vector<market::CSecurity>& securities, std::vector<market::CQuote>* scan = nullptr);
		static std::string MakeKey(const market::CChannelInfo& subscription);

	private:
		std::atomic_bool m_running{ false };
		std::thread m_thread;
		std::mutex m_wait_mutex;
		std::condition_variable m_wait_condition;
		mutable std::mutex m_state_mutex;
		std::unordered_map<std::string, market::CChannelInfo> m_subscriptions;
		std::vector<market::CSecurity> m_scan_securities;
		std::filesystem::path m_scan_calendar;
		_TyScanHandler m_scan_handler;
		std::uint64_t m_scan_revision{ 0 };
		_TyQuoteHandler m_quote_handler;
		_TyDepthHandler m_depth_handler;
		market::CProviderStatus m_status;
		std::array<std::int64_t, 1024> m_request_durations{};
		std::size_t m_duration_count{ 0 };
		std::size_t m_duration_cursor{ 0 };
		std::uint64_t m_subscription_failures{ 0 };
		std::array<std::int64_t, 1024> m_refresh_intervals{};
		std::size_t m_interval_count{ 0 };
		std::size_t m_interval_cursor{ 0 };
		std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_last_refresh;
		void* m_provider{ nullptr };
	};
} // namespace provider
#endif
