#ifndef __CAKSHARE_PROVIDER_H__
#define __CAKSHARE_PROVIDER_H__
#include "../quote/IMarketProvider.h"
#include <chrono>
#include <mutex>
#include <unordered_map>
namespace provider
{
	class CAkShareProvider final : public market::IMarketProvider
	{
	  public:
		~CAkShareProvider() override;
		const char* Name() const override;
		bool Initialize() override;
		bool Subscribe(const std::vector<market::CChannelInfo>&) override;
		bool Unsubscribe(const std::vector<market::CChannelInfo>&) override;
		std::vector<market::CBar> QueryBars(const market::CSecurity& security, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime) override;
		market::CProviderStatus GetStatus() const override;
		void SetQuoteHandler(_TyQuoteHandler) override;
		void SetDepthHandler(_TyDepthHandler) override;
		void Stop() override;
		std::vector<market::CInstrument> QueryInstruments() const;
		std::vector<market::CSector> QuerySectors(market::SectorType type);
		market::CSectorConstituents QuerySectorConstituents(market::SectorType type, const std::string& strSectorCode);

	  private:
		mutable std::mutex m_mtx_state;
		market::CProviderStatus m_status;
		void* m_pProvider{ nullptr };
		std::vector<market::CInstrument> m_instruments;
		std::vector<market::CSector> m_sectors;
		std::unordered_map<std::string, market::CSectorConstituents> m_sectorConstituents;
		std::chrono::steady_clock::time_point m_sectorExpiry;
		std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_sectorConstituentExpiry;
	};
} // namespace provider
#endif
