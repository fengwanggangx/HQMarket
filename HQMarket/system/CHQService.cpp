#include "CHQService.h"
#include "../common/utility.h"
#include "../python/CPythonRuntime.h"
#include "../request/request.h"
#include "../request/request.pb.h"
#include "../common/defines.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <unordered_set>
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
		result.m_code = value.substr(0, dot);
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
		value->mutable_security()->set_symbol(quote.m_security.m_code);
		value->mutable_security()->set_exchange(ToWire(quote.m_security.m_market));
		value->set_exchange_time_ms(quote.m_exchange_time);
		value->set_receive_time_ms(quote.m_receive_time);
		value->set_last_price(quote.m_last_price);
		value->set_open_price(quote.m_open_price);
		value->set_high_price(quote.m_high_price);
		value->set_low_price(quote.m_low_price);
		value->set_pre_close(quote.m_pre_close);
		value->set_volume(quote.m_volume);
		value->set_turnover(quote.m_turnover);
		value->set_price_scale(quote.m_price_scale);
		value->set_source(quote.m_source);
		value->set_stale(quote.m_stale);
	}

	void FillBar(const market::CBar& bar, wire::BarData* value)
	{
		value->mutable_security()->set_symbol(bar.m_security.m_code);
		value->mutable_security()->set_exchange(ToWire(bar.m_security.m_market));
		value->set_channel(static_cast<wire::Channel>(static_cast<int>(bar.m_channel)));
		value->set_begin_time_ms(bar.m_begin_time);
		value->set_end_time_ms(bar.m_end_time);
		value->set_completed(bar.m_completed);
		value->set_data_version(bar.m_data_version);
		value->set_basis_version(bar.m_basis_version);
		value->set_open_price(bar.m_open_price);
		value->set_high_price(bar.m_high_price);
		value->set_low_price(bar.m_low_price);
		value->set_close_price(bar.m_close_price);
		value->set_volume(bar.m_volume);
		value->set_turnover(bar.m_turnover);
		value->set_price_scale(bar.m_price_scale);
		value->set_adjustment(bar.m_adjustment);
		value->set_source(bar.m_source);
	}

	void FillSector(const market::CSector& sector, wire::SectorInfo* value)
	{
		value->set_code(sector.m_code);
		value->set_name(sector.m_name);
		value->set_change_percent(sector.m_change_percent);
		value->set_percent_scale(sector.m_percent_scale);
		value->set_rising_count(sector.m_rising_count);
		value->set_falling_count(sector.m_falling_count);
		value->set_flat_count(sector.m_flat_count);
		value->set_member_count(sector.m_member_count);
		value->mutable_leading_security()->set_symbol(sector.m_leading_security.m_code);
		value->mutable_leading_security()->set_exchange(ToWire(sector.m_leading_security.m_market));
		value->set_leading_name(sector.m_leading_name);
		value->set_snapshot_time_ms(sector.m_snapshot_time);
	}

	void FillSecurityInfo(const market::CInstrument& instrument, wire::SecurityInfo* value)
	{
		value->mutable_security()->set_symbol(instrument.m_security.m_code);
		value->mutable_security()->set_exchange(ToWire(instrument.m_security.m_market));
		value->set_name(instrument.m_name);
		value->set_status(instrument.m_status);
		for (const std::string& strAlias : instrument.m_pinyin_full_aliases)
		{
			value->add_pinyin_full_aliases(strAlias);
		}
		for (const std::string& strAlias : instrument.m_pinyin_short_aliases)
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

CHQService::CHQService(CPythonRuntime* pPythonRuntime) : m_python_runtime(pPythonRuntime)
{
}

CHQService::~CHQService()
{
	Stop();
}

bool CHQService::Initialize(const std::filesystem::path& root)
{
	return nullptr != m_python_runtime && m_python_runtime->IsInitialized() && m_broker.Initialize(root);
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
	std::lock_guard<std::recursive_mutex> lock(m_mtx_subscriptions);
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
	std::lock_guard<std::recursive_mutex> lock(m_mtx_subscriptions);
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
		pResult->mutable_security()->set_symbol(subscription.m_security.m_code);
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
			std::uint64_t sequence = 0;
			std::optional<market::CQuote> quote = m_broker.QueryQuote(subscription.m_security, &sequence);
			if (quote.has_value())
			{
				wire::QuoteData quoteData;
				FillQuote(*quote, &quoteData);
				CRequest snapshot;
				snapshot.SetType(_TyRequest::MARKET_QUOTE_PUSH);
				SetData(snapshot, quoteData, 0, sequence);
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
	if (_TyRequest::MARKET_QUOTE_QUERY == type && req.GetData().has_market_snapshot_query())
	{
		const wire::MarketSnapshotQuery& query = req.GetData().market_snapshot_query();
		std::uint32_t limit = query.has_limit() ? query.limit() : 100;
		const std::string& board = query.board();
		const std::string& scope = query.scope();
		bool validBoard = board.empty() || "gainers" == board || "losers" == board || "turnover" == board;
		bool validScope = scope.empty() || "a_share" == scope || "index" == scope || "fund" == scope || "convertible_bond" == scope || "hk" == scope;
		if (0 == limit || 500 < limit || (0 != query.offset() && 0 == query.version()) || !validBoard || !validScope)
		{
			SendError(id, req, 1003, "invalid snapshot query");
			return false;
		}
		_TyMarketSnapshotPage page;
		MarketSnapshotJson(query.version(), query.offset(), limit, board, scope, query.allow_partial(), &page);
		CRequest response;
		response.SetType(type);
		SetData(response, page, requestId);
		return SendResponse(id, response);
	}
	if (_TyRequest::MARKET_QUOTE_QUERY == type && "market_snapshot" == req.GetExtraData("view"))
	{
		std::int64_t version = 0;
		std::int64_t offset = 0;
		std::int64_t limit = 100;
		std::string limitValue = req.GetExtraData("limit");
		std::string board = req.GetExtraData("board");
		std::string scope = req.GetExtraData("scope");
		bool validBoard = board.empty() || "gainers" == board || "losers" == board || "turnover" == board;
		bool validScope = scope.empty() || "a_share" == scope || "index" == scope || "fund" == scope || "convertible_bond" == scope || "hk" == scope;
		if (!ParseMilliseconds(req.GetExtraData("version"), version) || !ParseMilliseconds(req.GetExtraData("offset"), offset) || (!limitValue.empty() && !ParseMilliseconds(limitValue, limit)) || 0 > version || 0 > offset || 0 >= limit || 500 < limit || (0 != offset && 0 == version) || !validBoard || !validScope)
		{
			SendError(id, req, 1003, "invalid snapshot query");
			return false;
		}
		CRequest response;
		response.SetType(type);
		response.SetId(requestId);
		response.SetExtraData("market_snapshot", MarketSnapshotJson(version, offset, limit, board, scope, "1" == req.GetExtraData("partial")));
		return SendResponse(id, response);
	}
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
			HashString(version, instrument.m_security.m_code);
			version = (version ^ static_cast<std::uint64_t>(instrument.m_security.m_market)) * 1099511628211ULL;
			HashString(version, instrument.m_name);
			HashString(version, instrument.m_status);
			for (const std::string& strAlias : instrument.m_pinyin_full_aliases)
			{
				HashString(version, strAlias);
			}
			for (const std::string& strAlias : instrument.m_pinyin_short_aliases)
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
		result.set_snapshot_time_ms(sectors.front().m_snapshot_time);
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
		result.set_snapshot_time_ms(constituents.m_snapshot_time);
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
	pResult->mutable_security()->set_symbol(security.m_code);
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
		std::int64_t effectiveEnd = 0 == end ? NowMilliseconds() : end;
		if ((0 > begin) || (0 > end) || (effectiveEnd < begin))
		{
			SetError(response, req, 1004, "invalid query time range");
			SendResponse(id, response);
			return false;
		}
		market::CProviderStatus history{ true, "" };
		std::vector<market::CBar> bars = m_broker.QueryBars(security, channel, begin, end, &history);
		if (bars.empty() && !history.m_healthy)
		{
			SetError(response, req, StorageUnavailable,
				history.m_detail.empty() ? "history provider is unavailable" : history.m_detail);
			SendResponse(id, response);
			return false;
		}
		if (market::Channel::bar_1m == channel)
		{
			response.SetExtraData("history_coverage", "unverified");
			if (!bars.empty())
			{
				response.SetExtraData("first_bar_time_ms", std::to_string(bars.front().m_begin_time));
				response.SetExtraData("last_bar_time_ms", std::to_string(bars.back().m_begin_time));
			}
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
	std::lock_guard<std::recursive_mutex> lock(m_mtx_subscriptions);
	if (m_broadcastHandler)
	{
		m_broadcastHandler(m_subscriptions.GetSubscriberIds(subscription), request);
	}
}

void CHQService::OnDepth(market::CDepth&& depth)
{
	std::uint64_t nSequence = ++m_depth_sequence;
	wire::DepthData depthData;
	wire::DepthData* pValue = &depthData;
	pValue->mutable_security()->set_symbol(depth.m_security.m_code);
	pValue->mutable_security()->set_exchange(ToWire(depth.m_security.m_market));
	pValue->set_exchange_time_ms(depth.m_exchange_time);
	pValue->set_receive_time_ms(depth.m_receive_time);
	pValue->set_source(depth.m_source);
	pValue->set_stale(depth.m_stale);
	for (const market::CPriceLevel& level : depth.m_bids)
	{
		wire::PriceLevel* pLevel = pValue->add_bids();
		pLevel->set_price(level.m_price);
		pLevel->set_volume(level.m_volume);
		pLevel->set_price_scale(level.m_price_scale);
	}
	for (const market::CPriceLevel& level : depth.m_asks)
	{
		wire::PriceLevel* pLevel = pValue->add_asks();
		pLevel->set_price(level.m_price);
		pLevel->set_volume(level.m_volume);
		pLevel->set_price_scale(level.m_price_scale);
	}
	CRequest request;
	request.SetType(_TyRequest::MARKET_DEPTH_PUSH);
	SetData(request, depthData, 0, nSequence);
	market::CChannelInfo subscription{ depth.m_security, market::Channel::depth };
	std::lock_guard<std::recursive_mutex> lock(m_mtx_subscriptions);
	if (m_broadcastHandler)
	{
		m_broadcastHandler(m_subscriptions.GetSubscriberIds(subscription), request);
	}
}

std::string CHQService::HealthJson() const
{
	market::CProviderStatus realtime = m_broker.RealtimeStatus();
	market::CProviderStatus history = m_broker.HistoryStatus();
	std::ostringstream out;
	out << "{\"status\":\"" << ((realtime.m_healthy && history.m_healthy) ? "ok" : "degraded")
		<< "\",\"python\":" << (((nullptr != m_python_runtime) && m_python_runtime->IsInitialized()) ? "true" : "false")
		<< ",\"sqlite\":" << (m_broker.IsRecorderOpen() ? "true" : "false")
		<< ",\"mootdx\":" << (realtime.m_healthy ? "true" : "false")
		<< ",\"akshare\":" << (history.m_healthy ? "true" : "false") << "}";
	return out.str();
}

std::string CHQService::MetricsText(std::size_t nClients) const
{
	std::ostringstream out;
	out << "hqmarket_clients " << nClients
		<< "\nhqmarket_quotes_cached " << m_broker.QuoteCount() << "\n";
	out << m_broker.MarketMetrics();
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
	out << "{\"security\":\"" << quote->m_security.String() << "\",\"exchange_time_ms\":" << quote->m_exchange_time
		<< ",\"receive_time_ms\":" << quote->m_receive_time << ",\"last_price\":" << quote->m_last_price
		<< ",\"price_scale\":" << quote->m_price_scale << ",\"volume\":" << quote->m_volume
		<< ",\"turnover\":" << quote->m_turnover << ",\"source\":\"" << quote->m_source
		<< "\",\"stale\":" << (quote->m_stale ? "true" : "false") << "}";
	return out.str();
}

std::string CHQService::MarketSnapshotJson(std::uint64_t version, std::size_t offset, std::size_t limit, const std::string& board, const std::string& scope, bool allowPartial, _TyMarketSnapshotPage* page) const
{
	std::optional<CHQBroker::_TyMarketSnapshot> snapshot = m_broker.QueryMarketSnapshot(version, "hk" == scope, !board.empty() && !allowPartial);
	if (!snapshot.has_value())
	{
		if (nullptr != page)
		{
			page->set_status(0 == version ? "preparing" : "version_expired");
			return {};
		}
		return 0 == version ? "{\"status\":\"preparing\"}" : "{\"status\":\"version_expired\"}";
	}
	std::unordered_set<market::CSecurity, market::CSecurityHash> halted;
	if (!scope.empty() || !board.empty())
	{
		std::string selected = scope.empty() ? "a_share" : scope;
		std::unordered_set<market::CSecurity, market::CSecurityHash> securities;
		for (const auto& instrument : snapshot->m_instruments)
		{
			bool matches = ("a_share" == selected && market::InstrumentKind::aShare == instrument.m_kind) || ("index" == selected && market::InstrumentKind::index == instrument.m_kind) || ("fund" == selected && market::InstrumentKind::fund == instrument.m_kind) || ("convertible_bond" == selected && market::InstrumentKind::convertibleBond == instrument.m_kind) || ("hk" == selected && market::Exchange::hkex == instrument.m_security.m_market && (board.empty() || market::InstrumentKind::hkStock == instrument.m_kind));
			if (matches)
			{
				securities.emplace(instrument.m_security);
				if ("halted" == instrument.m_status)
				{
					halted.emplace(instrument.m_security);
				}
			}
		}
		snapshot->m_expected = securities.size();
		std::erase_if(snapshot->m_quotes, [&securities](const market::CQuote& quote)
		{
			return !securities.contains(quote.m_security);
		});
	}
	else
	{
		for (const auto& instrument : snapshot->m_instruments)
		{
			if ("halted" == instrument.m_status)
			{
				halted.emplace(instrument.m_security);
			}
		}
	}
	std::size_t covered = halted.size();
	for (const auto& quote : snapshot->m_quotes)
	{
		if (!halted.contains(quote.m_security))
		{
			++covered;
		}
	}
	bool partial = 0 == snapshot->m_expected || covered < snapshot->m_expected;
	bool priorDate = market::MarketDateIndex(snapshot->m_finished_at) != market::MarketDateIndex(NowMilliseconds());
	if (!board.empty() && "hk" == scope && !snapshot->m_directory_verified)
	{
		partial = true;
	}
	bool boardPreparing = !board.empty() && (0 == snapshot->m_expected || priorDate || ((partial || !snapshot->m_directory_verified) && !allowPartial));
	std::size_t received = snapshot->m_quotes.size();
	std::vector<market::CQuote>& quotes = snapshot->m_quotes;
	std::size_t unknownExchangeTime = std::ranges::count_if(quotes, [](const market::CQuote& quote)
	{
		return 0 == quote.m_exchange_time;
	});
	if (boardPreparing)
	{
		quotes.clear();
	}
	else if (!board.empty())
	{
		std::erase_if(quotes, [&board, &halted](const market::CQuote& quote)
		{
			if (0 >= quote.m_last_price || halted.contains(quote.m_security))
			{
				return true;
			}
			if ("turnover" == board)
			{
				return 0 >= quote.m_turnover;
			}
			return 0 >= quote.m_pre_close || ("gainers" == board ? quote.m_last_price <= quote.m_pre_close : quote.m_last_price >= quote.m_pre_close);
		});
	}
	std::ranges::sort(quotes, [&board](const market::CQuote& left, const market::CQuote& right)
	{
		if ("turnover" == board && left.m_turnover != right.m_turnover)
		{
			return left.m_turnover > right.m_turnover;
		}
		if ("gainers" == board || "losers" == board)
		{
			long double leftRatio = static_cast<long double>(left.m_last_price) / left.m_pre_close;
			long double rightRatio = static_cast<long double>(right.m_last_price) / right.m_pre_close;
			if (leftRatio != rightRatio)
			{
				return "gainers" == board ? leftRatio > rightRatio : leftRatio < rightRatio;
			}
		}
		return left.m_security.m_market != right.m_security.m_market ? left.m_security.m_market < right.m_security.m_market : left.m_security.m_code < right.m_security.m_code;
	});

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("version");
	writer.Uint64(snapshot->m_version);
	writer.Key("expected");
	writer.Uint64(snapshot->m_expected);
	writer.Key("received");
	writer.Uint64(received);
	writer.Key("missing");
	writer.Uint64(snapshot->m_expected - (std::min)(covered, snapshot->m_expected));
	writer.Key("confirmed_halted");
	writer.Uint64(halted.size());
	writer.Key("unknown_exchange_time");
	writer.Uint64(unknownExchangeTime);
	writer.Key("started_at_ms");
	writer.Int64(snapshot->m_started_at);
	writer.Key("finished_at_ms");
	writer.Int64(snapshot->m_finished_at);
	writer.Key("partial");
	writer.Bool(partial);
	writer.Key("directory_verified");
	writer.Bool(snapshot->m_directory_verified);
	writer.Key("prior_date");
	writer.Bool(priorDate);
	writer.Key("status");
	writer.String(boardPreparing ? "preparing" : "ready");
	std::size_t start = (std::min)(offset, quotes.size());
	std::size_t end = start + (std::min)(limit, quotes.size() - start);
	if (nullptr != page)
	{
		page->set_status(boardPreparing ? "preparing" : "ready");
		page->set_version(snapshot->m_version);
		page->set_expected(snapshot->m_expected);
		page->set_received(received);
		page->set_missing(snapshot->m_expected - (std::min)(covered, snapshot->m_expected));
		page->set_confirmed_halted(halted.size());
		page->set_unknown_exchange_time(unknownExchangeTime);
		page->set_started_at_ms(snapshot->m_started_at);
		page->set_finished_at_ms(snapshot->m_finished_at);
		page->set_partial(partial);
		page->set_directory_verified(snapshot->m_directory_verified);
		page->set_prior_date(priorDate);
		page->set_turnover_scale(2);
		if (end < quotes.size())
		{
			page->set_next_offset(end);
		}
		for (std::size_t index = start; index < end; ++index)
		{
			FillQuote(quotes[index], page->add_quotes());
		}
		return {};
	}
	writer.Key("next_offset");
	if (end < quotes.size())
	{
		writer.Uint64(end);
	}
	else
	{
		writer.Null();
	}
	writer.Key("quotes");
	writer.StartArray();
	for (std::size_t index = start; index < end; ++index)
	{
		const market::CQuote& quote = quotes[index];
		std::string security = quote.m_security.String();
		writer.StartObject();
		writer.Key("security");
		writer.String(security.c_str());
		writer.Key("last_price");
		writer.Int64(quote.m_last_price);
		writer.Key("pre_close");
		writer.Int64(quote.m_pre_close);
		writer.Key("price_scale");
		writer.Int(quote.m_price_scale);
		writer.Key("open_price");
		writer.Int64(quote.m_open_price);
		writer.Key("high_price");
		writer.Int64(quote.m_high_price);
		writer.Key("low_price");
		writer.Int64(quote.m_low_price);
		writer.Key("volume");
		writer.Int64(quote.m_volume);
		writer.Key("turnover");
		writer.Int64(quote.m_turnover);
		writer.Key("turnover_scale");
		writer.Int(2);
		writer.Key("exchange_time_ms");
		writer.Int64(quote.m_exchange_time);
		writer.Key("receive_time_ms");
		writer.Int64(quote.m_receive_time);
		writer.Key("source");
		writer.String(quote.m_source.c_str());
		writer.EndObject();
	}
	writer.EndArray();
	writer.EndObject();
	return buffer.GetString();
}

std::string CHQService::InstrumentsJson() const
{
	std::vector<market::CInstrument> values = m_broker.QueryInstruments();
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartArray();
	for (const market::CInstrument& instrument : values)
	{
		std::string security = instrument.m_security.String();
		writer.StartObject();
		writer.Key("security");
		writer.String(security.data(), static_cast<rapidjson::SizeType>(security.size()));
		writer.Key("name");
		writer.String(instrument.m_name.data(), static_cast<rapidjson::SizeType>(instrument.m_name.size()));
		writer.Key("status");
		writer.String(instrument.m_status.data(), static_cast<rapidjson::SizeType>(instrument.m_status.size()));
		writer.EndObject();
	}
	writer.EndArray();
	return buffer.GetString();
}

std::string CHQService::BarsJson(const std::string& strInstrument, const std::string& strChannel, const std::string& strBeginTime, const std::string& strEndTime, int& status, std::optional<std::pair<std::int64_t, std::int64_t>>& returnedRange)
{
	market::CSecurity security = ParseInstrument(strInstrument);
	market::Channel channel = strChannel.empty() ? market::Channel::bar_1d : ParseChannel(strChannel);
	if (security.m_code.empty() || market::Exchange::unknown == security.m_market ||
		(market::Channel::bar_1d != channel && market::Channel::bar_1m != channel))
	{
		status = 400;
		return R"({"error":"invalid instrument or bar channel"})";
	}
	std::int64_t begin = 0;
	std::int64_t end = 0;
	if ((!strBeginTime.empty() && !ParseMilliseconds(strBeginTime, begin)) ||
		(!strEndTime.empty() && !ParseMilliseconds(strEndTime, end)))
	{
		status = 400;
		return R"({"error":"invalid query time range"})";
	}
	std::int64_t effectiveEnd = 0 == end ? NowMilliseconds() : end;
	if (0 > begin || 0 > end || effectiveEnd < begin)
	{
		status = 400;
		return R"({"error":"invalid query time range"})";
	}
	market::CProviderStatus history{ true, "" };
	std::vector<market::CBar> values = m_broker.QueryBars(security, channel, begin, end, &history);
	if (values.empty() && !history.m_healthy)
	{
		status = 503;
		return R"({"error":"history provider is unavailable"})";
	}
	status = 200;
	if (!values.empty())
	{
		returnedRange.emplace(values.front().m_begin_time, values.back().m_begin_time);
	}
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
		out << "{\"begin_time_ms\":" << bar.m_begin_time << ",\"open\":" << bar.m_open_price
			<< ",\"high\":" << bar.m_high_price << ",\"low\":" << bar.m_low_price << ",\"close\":" << bar.m_close_price
			<< ",\"volume\":" << bar.m_volume << ",\"price_scale\":" << bar.m_price_scale << "}";
	}
	out << ']';
	return out.str();
}

void CHQService::SetBroadcastHandler(_TyBroadcastHandler&& handler)
{
	m_broadcastHandler = std::move(handler);
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
