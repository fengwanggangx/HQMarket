#ifndef __CMARKET_SERVICE_H__
#define __CMARKET_SERVICE_H__
#include "../market/CMarketCache.h"
#include "../network/CMarketTcpServer.h"
#include "../python/CMooTdxProvider.h"
#include "../python/CAkShareProvider.h"
#include "../python/CPythonRuntime.h"
#include "../storage/CMarketStorage.h"
#include <filesystem>
#include <atomic>
#include <memory>
#include <string>
namespace service
{
	class CMarketService final
	{
		public:
			bool Initialize(int nTcpPort, const std::string& strToken, const std::filesystem::path& root);
			void Run();
			void Stop();
			std::string HealthJson() const;
			std::string MetricsText() const;
			std::string QuoteJson(const std::string& strInstrument) const;
			std::string InstrumentsJson() const;
			std::string BarsJson(const std::string& strInstrument, market::Channel channel, std::int64_t nBeginTime,
								 std::int64_t nEndTime);

		private:
			provider::CPythonRuntime m_python;
			provider::CMooTdxProvider m_mootdx;
			provider::CAkShareProvider m_akshare;
			market::CMarketCache m_cache;
			storage::CMarketStorage m_storage;
			std::unique_ptr<net::CMarketTcpServer> m_pTcpServer;
			std::atomic_uint64_t m_nDepthSequence{0};
	};
} // namespace service
#endif
