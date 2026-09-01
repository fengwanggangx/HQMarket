#ifndef __CHQ_BROKER_H__
#define __CHQ_BROKER_H__

#include "CHQCache.h"
#include "CHQRecorder.h"
#include "../provider/CAkShareProvider.h"
#include "../provider/CMooTdxProvider.h"
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

		~CHQBroker();
		bool Initialize(const std::filesystem::path& root);
		void Stop();
		bool Subscribe(const std::vector<market::CChannelInfo>& infos);
		bool Unsubscribe(const std::vector<market::CChannelInfo>& infos);
		std::optional<market::CQuote> QueryQuote(const market::CSecurity& security) const;
		std::vector<market::CBar> QueryBars(const market::CSecurity& security, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime);
		std::vector<market::CSecurity> QueryInstruments() const;
		market::CProviderStatus RealtimeStatus() const;
		market::CProviderStatus HistoryStatus() const;
		bool IsRecorderOpen() const;
		std::size_t QuoteCount() const;
		void SetQuoteHandler(_TyQuoteHandler handler);
		void SetDepthHandler(_TyDepthHandler handler);

	private:
		provider::CMooTdxProvider m_mootdx;
		provider::CAkShareProvider m_akshare;
		CHQCache m_cache;
		CHQRecorder m_recorder;
		_TyQuoteHandler m_quoteHandler;
		_TyDepthHandler m_depthHandler;
};

#endif
