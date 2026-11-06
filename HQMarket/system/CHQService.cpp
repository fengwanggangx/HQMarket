#include "CHQService.h"
#include "../common/utility.h"
#include "../python/CPythonRuntime.h"
#include "../request/request.h"
#include "../request/request.pb.h"
#include "../common/defines.h"
#include <chrono>
#include <cctype>
#include <exception>
#include <sstream>
#include <utility>

namespace wire = hqmarket::market::v1;

namespace
{
	constexpr int StorageUnavailable = 1004;

	std::int64_t NowMilliseconds()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count();
	}

	wire::Exchange ToWire(market::Exchange value)
	{
		return static_cast<wire::Exchange>(static_cast<int>(value));
	}

	market::CSecurity ParseInstrument(const std::string& value)
	{
		market::CSecurity result;
		std::size_t dot = value.rfind('.');
		if (std::string::npos == dot)
		{
			return result;
		}
		result.m_strCode = value.substr(0, dot);
		std::string exchange = value.substr(dot + 1);
		if ("SSE" == exchange)
		{
			result.m_market = market::Exchange::sse;
		}
		else if ("SZSE" == exchange)
		{
			result.m_market = market::Exchange::szse;
		}
		else if ("BSE" == exchange)
		{
			result.m_market = market::Exchange::bse;
		}
		else if ("HKEX" == exchange)
		{
			result.m_market = market::Exchange::hkex;
		}
		return result;
	}

	bool IsRealtimeChannel(market::Channel channel)
	{
		return (market::Channel::quote == channel) || (market::Channel::depth == channel);
	}

	market::Channel ParseChannel(const std::string& v)
	{
		if ("quote" == v)
		{
			return market::Channel::quote;
		}
		if ("depth" == v)
		{
			return market::Channel::depth;
		}
		if ("bar_1m" == v)
		{
			return market::Channel::bar_1m;
		}
		if ("bar_1d" == v)
		{
			return market::Channel::bar_1d;
		}
		return market::Channel::unknown;
	}

	bool ParseMilliseconds(const std::string& value, std::int64_t& result)
	{
		try
		{
			std::size_t parsed = 0;
			result = std::stoll(value, &parsed);
			return parsed == value.size();
		}
		catch (const std::exception&)
		{
			return false;
		}
	}

	void FillQuote(const market::CQuote& quote, wire::QuoteData* value)
	{
		value->mutable_security()->set_symbol(quote.m_security.m_strCode);
		value->mutable_security()->set_exchange(ToWire(quote.m_security.m_market));
		value->set_exchange_time_ms(quote.m_nExchangeTime);
		value->set_receive_time_ms(quote.m_nReceiveTime);
		value->set_last_price(quote.m_nLastPrice);
		value->set_open_price(quote.m_nOpenPrice);
		value->set_high_price(quote.m_nHighPrice);
		value->set_low_price(quote.m_nLowPrice);
		value->set_pre_close(quote.m_nPreClose);
		value->set_volume(quote.m_nVolume);
		value->set_turnover(quote.m_nTurnover);
		value->set_price_scale(quote.m_nPriceScale);
		value->set_source(quote.m_strSource);
		value->set_stale(quote.m_bStale);
	}

	void FillBar(const market::CBar& bar, wire::BarData* value)
	{
		value->mutable_security()->set_symbol(bar.m_security.m_strCode);
		value->mutable_security()->set_exchange(ToWire(bar.m_security.m_market));
		value->set_channel(static_cast<wire::Channel>(static_cast<int>(bar.m_channel)));
		value->set_begin_time_ms(bar.m_nBeginTime);
		value->set_open_price(bar.m_nOpenPrice);
		value->set_high_price(bar.m_nHighPrice);
		value->set_low_price(bar.m_nLowPrice);
		value->set_close_price(bar.m_nClosePrice);
		value->set_volume(bar.m_nVolume);
		value->set_turnover(bar.m_nTurnover);
		value->set_price_scale(bar.m_nPriceScale);
		value->set_adjustment(bar.m_strAdjustment);
		value->set_source(bar.m_strSource);
	}

	void FillSector(const market::CSector& sector, wire::SectorInfo* value)
	{
		value->set_code(sector.m_strCode);
		value->set_name(sector.m_strName);
		value->set_change_percent(sector.m_nChangePercent);
		value->set_percent_scale(sector.m_nPercentScale);
		value->set_rising_count(sector.m_nRisingCount);
		value->set_falling_count(sector.m_nFallingCount);
		value->set_flat_count(sector.m_nFlatCount);
		value->set_member_count(sector.m_nMemberCount);
		value->mutable_leading_security()->set_symbol(sector.m_leadingSecurity.m_strCode);
		value->mutable_leading_security()->set_exchange(ToWire(sector.m_leadingSecurity.m_market));
		value->set_leading_name(sector.m_strLeadingName);
		value->set_snapshot_time_ms(sector.m_nSnapshotTime);
	}

	void FillSecurityInfo(const market::CInstrument& instrument, wire::SecurityInfo* value)
	{
		value->mutable_security()->set_symbol(instrument.m_security.m_strCode);
		value->mutable_security()->set_exchange(ToWire(instrument.m_security.m_market));
		value->set_name(instrument.m_strName);
		value->set_status(instrument.m_strStatus);
		for (const std::string& strAlias : instrument.m_pinyinFullAliases)
		{
			value->add_pinyin_full_aliases(strAlias);
		}
		for (const std::string& strAlias : instrument.m_pinyinShortAliases)
		{
			value->add_pinyin_short_aliases(strAlias);
		}
	}

	void HashString(std::uint64_t& value, const std::string& strText)
	{
		for (unsigned char character : strText)
		{
			value = (value ^ character) * 1099511628211ULL;
		}
		value = (value ^ 0xffU) * 1099511628211ULL;
	}

	void SetError(CRequest& response, const CRequest& request, int code, const std::string& message)
	{
		response = CRequest();
		response.SetId(request.GetId());
		response.SetType(request.GetType());
		if ((_TyRequest::MARKET_SUBSCRIBE == request.GetType()) || (_TyRequest::MARKET_UNSUBSCRIBE == request.GetType()))
		{
			response.SetExtraData("security", request.GetExtraData("security"));
			response.SetExtraData("channel", request.GetExtraData("channel"));
		}
		response.SetErrorInfo(code, message);
	}

	template <typename T>
	bool SetData(CRequest& request, const T& value, std::uint64_t requestId = 0, std::uint64_t sequence = 0)
	{
		request.SetId(requestId);
		request.SetExtraData("request_id", std::to_string(requestId));
		request.SetExtraData("sequence", std::to_string(sequence));
		request.SetExtraData("server_time_ms", std::to_string(NowMilliseconds()));
		request.SetData(value);
		return true;
	}

} // namespace

CHQService::CHQService(CPythonRuntime* pPythonRuntime) : m_pPythonRuntime(pPythonRuntime)
{
}

CHQService::~CHQService()
{
	Stop();
}

bool CHQService::Initialize(const std::filesystem::path& root)
{
	return nullptr != m_pPythonRuntime && m_pPythonRuntime->IsInitialized() && m_broker.Initialize(root);
}

void CHQService::Stop()
{
	m_broker.Stop();
}

bool CHQService::OnRequest(const CRequest& req)
{
	switch (req.GetType())
	{
	case _TyRequest::MARKET_SUBSCRIBE:
	case _TyRequest::MARKET_UNSUBSCRIBE:
		return HandleSubscription(req.GetConnectionId(), req);
	case _TyRequest::MARKET_QUOTE_QUERY:
	case _TyRequest::MARKET_BARS_QUERY:
	case _TyRequest::MARKET_SECURITIES_QUERY:
	case _TyRequest::MARKET_SECTORS_QUERY:
	case _TyRequest::MARKET_SECTOR_CONSTITUENTS_QUERY:
		return HandleQuery(req.GetConnectionId(), req);
	default:
		SendError(req.GetConnectionId(), req, 1006, "unknown request type");
		return false;
	}
}

void CHQService::HandleClientDisconnected(net::_TyConnectionId id)
{
	std::vector<market::CChannelInfo> removed = m_subscriptions.RemoveClient(id);
	if (!removed.empty())
	{
		m_broker.Unsubscribe(removed);
	}
}

bool CHQService::HandleSubscription(net::_TyConnectionId id, const CRequest& req)
{
	_TyRequest type = req.GetType();
	if ((_TyRequest::MARKET_SUBSCRIBE != type) && (_TyRequest::MARKET_UNSUBSCRIBE != type))
	{
		SendError(id, req, 1001, "invalid subscription type");
		return false;
	}

	bool bSubscribe = _TyRequest::MARKET_SUBSCRIBE == type;
	std::uint64_t requestId = req.GetId();
	std::vector<market::CChannelInfo> requested;
	if (!req.GetExtraData("subscriptions").empty())
	{
		CRequest::_TyExtraRows rows;
		if (!req.GetExtraData("subscriptions", rows) || rows.empty())
		{
			SendError(id, req, 1003, "subscriptions must be a nonempty JSON array");
			return false;
		}
		requested.reserve(rows.size());
		for (const auto& row : rows)
		{
			auto securityValue = row.find("security");
			auto channelValue = row.find("channel");
			if ((row.end() == securityValue) || (row.end() == channelValue))
			{
				SendError(id, req, 1003, "subscription row requires security and channel");
				return false;
			}
			market::CSecurity security = ParseInstrument(securityValue->second);
			market::Channel channel = ParseChannel(channelValue->second);
			if (!security.IsValid() || !IsRealtimeChannel(channel))
			{
				SendError(id, req, 1003, "invalid subscription row");
				return false;
			}
			requested.emplace_back(market::CChannelInfo{ security, channel });
		}
	}
	else
	{
		market::CSecurity security = ParseInstrument(req.GetExtraData("security"));
		market::Channel channel = ParseChannel(req.GetExtraData("channel"));
		if (security.IsValid() && IsRealtimeChannel(channel))
		{
			requested.emplace_back(market::CChannelInfo{ security, channel });
		}
	}
	if (requested.empty())
	{
		SendError(id, req, 1003, "invalid security or unsupported subscription channel");
		return false;
	}
	std::vector<market::CChannelInfo> changed = bSubscribe ? m_subscriptions.Subscribe(id, requested) : m_subscriptions.Unsubscribe(id, requested);
	if (!changed.empty())
	{
		if (bSubscribe)
		{
			m_broker.Subscribe(changed);
		}
		else
		{
			m_broker.Unsubscribe(changed);
		}
	}

	wire::SubscriptionAck ack;
	for (const market::CChannelInfo& subscription : requested)
	{
		wire::SubscriptionResult* pResult = ack.add_results();
		pResult->mutable_security()->set_symbol(subscription.m_security.m_strCode);
		pResult->mutable_security()->set_exchange(ToWire(subscription.m_security.m_market));
		pResult->set_channel(static_cast<wire::Channel>(static_cast<int>(subscription.m_channel)));
		pResult->set_accepted(true);
	}

	CRequest response;
	response.SetType(req.GetType());
	response.SetErrorInfo(0, "");
	SetData(response, ack, requestId);
	SendResponse(id, response);
	if (bSubscribe)
	{
		for (const market::CChannelInfo& subscription : requested)
		{
			if (market::Channel::quote != subscription.m_channel)
			{
				continue;
			}
			std::optional<market::CQuote> quote = m_broker.QueryQuote(subscription.m_security);
			if (quote.has_value())
			{
				wire::QuoteData quoteData;
				FillQuote(*quote, &quoteData);
				CRequest snapshot;
				snapshot.SetType(_TyRequest::MARKET_QUOTE_PUSH);
				SetData(snapshot, quoteData);
				SendResponse(id, snapshot);
			}
		}
	}
	return true;
}

bool CHQService::HandleQuery(net::_TyConnectionId id, const CRequest& req)
{
	std::uint64_t requestId = req.GetId();
	_TyRequest type = req.GetType();
	if (_TyRequest::MARKET_SECURITIES_QUERY == type)
	{
		wire::SecurityList result;
		std::vector<market::CInstrument> instruments = m_broker.QueryInstruments();
		if (instruments.empty())
		{
			SendError(id, req, StorageUnavailable, "security list is unavailable");
			return false;
		}
		std::uint64_t version = 1469598103934665603ULL;
		for (const market::CInstrument& instrument : instruments)
		{
			wire::SecurityInfo* pInfo = result.add_securities();
			FillSecurityInfo(instrument, pInfo);
			HashString(version, instrument.m_security.m_strCode);
			version = (version ^ static_cast<std::uint64_t>(instrument.m_security.m_market)) * 1099511628211ULL;
			HashString(version, instrument.m_strName);
			HashString(version, instrument.m_strStatus);
			for (const std::string& strAlias : instrument.m_pinyinFullAliases)
			{
				HashString(version, strAlias);
			}
			for (const std::string& strAlias : instrument.m_pinyinShortAliases)
			{
				HashString(version, strAlias);
			}
		}
		result.set_version(static_cast<std::int64_t>(version));
		CRequest response;
		response.SetType(type);
		SetData(response, result, requestId);
		return SendResponse(id, response);
	}
	if (_TyRequest::MARKET_SECTORS_QUERY == type)
	{
		int nSectorType = 0;
		if (!utility::to_number(req.GetExtraData("sector_type"), nSectorType) || (static_cast<int>(market::SectorType::industry) != nSectorType))
		{
			SendError(id, req, 1003, "unsupported sector type");
			return false;
		}
		std::vector<market::CSector> sectors = m_broker.QuerySectors(market::SectorType::industry);
		if (sectors.empty())
		{
			SendError(id, req, StorageUnavailable, "sector data is unavailable");
			return false;
		}
		wire::SectorList result;
		result.set_type(wire::SECTOR_TYPE_INDUSTRY);
		result.set_snapshot_time_ms(sectors.front().m_nSnapshotTime);
		result.set_source("akshare");
		for (const auto& sector : sectors)
		{
			FillSector(sector, result.add_sectors());
		}
		CRequest response;
		response.SetType(type);
		SetData(response, result, requestId);
		return SendResponse(id, response);
	}
	if (_TyRequest::MARKET_SECTOR_CONSTITUENTS_QUERY == type)
	{
		int nSectorType = 0;
		std::string strSectorCode = req.GetExtraData("sector_code");
		if (!utility::to_number(req.GetExtraData("sector_type"), nSectorType) || (static_cast<int>(market::SectorType::industry) != nSectorType) || strSectorCode.empty())
		{
			SendError(id, req, 1003, "invalid sector constituent request");
			return false;
		}
		market::CSectorConstituents constituents = m_broker.QuerySectorConstituents(market::SectorType::industry, strSectorCode);
		if (constituents.m_securities.empty())
		{
			SendError(id, req, StorageUnavailable, "sector constituents are unavailable");
			return false;
		}
		wire::SectorConstituents result;
		result.set_type(wire::SECTOR_TYPE_INDUSTRY);
		FillSector(constituents.m_sector, result.mutable_sector());
		result.set_snapshot_time_ms(constituents.m_nSnapshotTime);
		result.set_source("akshare");
		for (const auto& instrument : constituents.m_securities)
		{
			wire::SecurityInfo* pInfo = result.add_securities();
			FillSecurityInfo(instrument, pInfo);
		}
		CRequest response;
		response.SetType(type);
		SetData(response, result, requestId);
		return SendResponse(id, response);
	}

	market::CSecurity security = ParseInstrument(req.GetExtraData("security"));
	market::Channel channel = _TyRequest::MARKET_QUOTE_QUERY == type ? market::Channel::quote : ParseChannel(req.GetExtraData("channel"));
	if (!security.IsValid() || ((market::Channel::quote != channel) && (market::Channel::bar_1m != channel) && (market::Channel::bar_1d != channel)))
	{
		SendError(id, req, 1003, "invalid security or unsupported query channel");
		return false;
	}
	wire::QueryResponse result;
	wire::QueryResponse* pResult = &result;
	pResult->mutable_security()->set_symbol(security.m_strCode);
	pResult->mutable_security()->set_exchange(ToWire(security.m_market));
	pResult->set_channel(static_cast<wire::Channel>(static_cast<int>(channel)));

	CRequest response;
	if (market::Channel::quote == channel)
	{
		std::optional<market::CQuote> quote = m_broker.QueryQuote(security);
		pResult->set_found(quote.has_value());
		if (quote.has_value())
		{
			FillQuote(*quote, pResult->mutable_quote());
		}
	}
	else
	{
		std::int64_t begin = 0;
		std::int64_t end = 0;
		if (!ParseMilliseconds(req.GetExtraData("begin_time_ms"), begin) || !ParseMilliseconds(req.GetExtraData("end_time_ms"), end))
		{
			SetError(response, req, 1004, "invalid query time range");
			SendResponse(id, response);
			return false;
		}
		end = 0 < end ? end : NowMilliseconds();
		if ((0 > begin) || (end < begin))
		{
			SetError(response, req, 1004, "invalid query time range");
			SendResponse(id, response);
			return false;
		}
		std::vector<market::CBar> bars = m_broker.QueryBars(security, channel, begin, end);
		market::CProviderStatus history = m_broker.HistoryStatus();
		if (bars.empty() && !history.m_bHealthy)
		{
			SetError(response, req, StorageUnavailable,
				history.m_strDetail.empty() ? "history provider is unavailable" : history.m_strDetail);
			SendResponse(id, response);
			return false;
		}
		pResult->set_found(!bars.empty());
		for (const market::CBar& bar : bars)
		{
			FillBar(bar, pResult->add_bars());
		}
	}
	response.SetType(type);
	SetData(response, result, requestId);
	SendResponse(id, response);
	return true;
}

void CHQService::OnQuote(const market::CQuote& quote, std::uint64_t nSequence)
{
	wire::QuoteData quoteData;
	FillQuote(quote, &quoteData);
	CRequest request;
	request.SetType(_TyRequest::MARKET_QUOTE_PUSH);
	SetData(request, quoteData, 0, nSequence);
	market::CChannelInfo subscription{ quote.m_security, market::Channel::quote };
	for (net::_TyConnectionId id : m_subscriptions.GetSubscriberIds(subscription))
	{
		SendResponse(id, request);
	}
}

void CHQService::OnDepth(market::CDepth&& depth)
{
	std::uint64_t nSequence = ++m_nDepthSequence;
	wire::DepthData depthData;
	wire::DepthData* pValue = &depthData;
	pValue->mutable_security()->set_symbol(depth.m_security.m_strCode);
	pValue->mutable_security()->set_exchange(ToWire(depth.m_security.m_market));
	pValue->set_exchange_time_ms(depth.m_nExchangeTime);
	pValue->set_receive_time_ms(depth.m_nReceiveTime);
	pValue->set_source(depth.m_strSource);
	pValue->set_stale(depth.m_bStale);
	for (const market::CPriceLevel& level : depth.m_bids)
	{
		wire::PriceLevel* pLevel = pValue->add_bids();
		pLevel->set_price(level.m_nPrice);
		pLevel->set_volume(level.m_nVolume);
		pLevel->set_price_scale(level.m_nPriceScale);
	}
	for (const market::CPriceLevel& level : depth.m_asks)
	{
		wire::PriceLevel* pLevel = pValue->add_asks();
		pLevel->set_price(level.m_nPrice);
		pLevel->set_volume(level.m_nVolume);
		pLevel->set_price_scale(level.m_nPriceScale);
	}
	CRequest request;
	request.SetType(_TyRequest::MARKET_DEPTH_PUSH);
	SetData(request, depthData, 0, nSequence);
	market::CChannelInfo subscription{ depth.m_security, market::Channel::depth };
	for (net::_TyConnectionId id : m_subscriptions.GetSubscriberIds(subscription))
	{
		SendResponse(id, request);
	}
}

std::string CHQService::HealthJson() const
{
	market::CProviderStatus realtime = m_broker.RealtimeStatus();
	market::CProviderStatus history = m_broker.HistoryStatus();
	std::ostringstream out;
	out << "{\"status\":\"" << ((realtime.m_bHealthy && history.m_bHealthy) ? "ok" : "degraded")
		<< "\",\"python\":" << (((nullptr != m_pPythonRuntime) && m_pPythonRuntime->IsInitialized()) ? "true" : "false")
		<< ",\"sqlite\":" << (m_broker.IsRecorderOpen() ? "true" : "false")
		<< ",\"mootdx\":" << (realtime.m_bHealthy ? "true" : "false")
		<< ",\"akshare\":" << (history.m_bHealthy ? "true" : "false") << "}";
	return out.str();
}

std::string CHQService::MetricsText(std::size_t nClients) const
{
	std::ostringstream out;
	out << "hqmarket_clients " << nClients
		<< "\nhqmarket_quotes_cached " << m_broker.QuoteCount() << "\n";
	return out.str();
}

std::string CHQService::QuoteJson(const std::string& strInstrument) const
{
	std::optional<market::CQuote> quote = m_broker.QueryQuote(ParseInstrument(strInstrument));
	if (!quote.has_value())
	{
		return "{}";
	}
	std::ostringstream out;
	out << "{\"security\":\"" << quote->m_security.String() << "\",\"exchange_time_ms\":" << quote->m_nExchangeTime
		<< ",\"receive_time_ms\":" << quote->m_nReceiveTime << ",\"last_price\":" << quote->m_nLastPrice
		<< ",\"price_scale\":" << quote->m_nPriceScale << ",\"volume\":" << quote->m_nVolume
		<< ",\"turnover\":" << quote->m_nTurnover << ",\"source\":\"" << quote->m_strSource
		<< "\",\"stale\":" << (quote->m_bStale ? "true" : "false") << "}";
	return out.str();
}

std::string CHQService::InstrumentsJson() const
{
	std::vector<market::CInstrument> values = m_broker.QueryInstruments();
	std::ostringstream out;
	out << '[';
	bool bFirst = true;
	for (const market::CInstrument& instrument : values)
	{
		if (!bFirst)
		{
			out << ',';
		}
		bFirst = false;
		out << "{\"security\":\"" << instrument.m_security.String() << "\",\"name\":\"" << instrument.m_strName << "\",\"status\":\"" << instrument.m_strStatus << "\"}";
	}
	out << ']';
	return out.str();
}

std::string CHQService::BarsJson(const std::string& strInstrument, market::Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime)
{
	market::CSecurity security = ParseInstrument(strInstrument);
	std::vector<market::CBar> values = m_broker.QueryBars(security, channel, nBeginTime, nEndTime);
	std::ostringstream out;
	out << '[';
	bool bFirst = true;
	for (const market::CBar& bar : values)
	{
		if (!bFirst)
		{
			out << ',';
		}
		bFirst = false;
		out << "{\"begin_time_ms\":" << bar.m_nBeginTime << ",\"open\":" << bar.m_nOpenPrice
			<< ",\"high\":" << bar.m_nHighPrice << ",\"low\":" << bar.m_nLowPrice << ",\"close\":" << bar.m_nClosePrice
			<< ",\"volume\":" << bar.m_nVolume << ",\"price_scale\":" << bar.m_nPriceScale << "}";
	}
	out << ']';
	return out.str();
}

void CHQService::SetReplyHandler(_TyReplyHandler&& handler)
{
	m_handler_reply = std::move(handler);
}

void CHQService::SetQuoteHandler(_TyQuoteHandler&& handler)
{
	m_broker.SetQuoteHandler(std::move(handler));
}

void CHQService::SetDepthHandler(_TyDepthHandler&& handler)
{
	m_broker.SetDepthHandler(std::move(handler));
}

bool CHQService::SendResponse(net::_TyConnectionId id, const CRequest& req) const
{
	return nullptr != m_handler_reply && m_handler_reply(id, req);
}

void CHQService::SendError(net::_TyConnectionId id, const CRequest& req, int code, const std::string& message) const
{
	CRequest response;
	SetError(response, req, code, message);
	SendResponse(id, response);
}
