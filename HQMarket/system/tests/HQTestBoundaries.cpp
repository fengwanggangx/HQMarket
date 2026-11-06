// Offline boundaries: the real center, service, broker, cache and SQLite recorder are linked.
// No TCP connections, Python interpreter or external market data are started by this executable.
#include "HQTestBoundaries.h"
#include "../../network/CHttpServer.h"
#include "../../network/CNetPool.h"
#include "../../network/CNetTools.h"
#include "../../provider/CMooTdxProvider.h"
#include "../../provider/CAkShareProvider.h"
#include "../../python/CPythonRuntime.h"
#include <utility>

namespace hqtest
{
	CBoundaryState& State()
	{
		static CBoundaryState state;
		return state;
	}
} // namespace hqtest

namespace net
{
	CNet::CNet() = default;
	CNet::~CNet() = default;
	CHttpServer::CHttpServer(int nPort) : m_nPort(nPort)
	{
	}
	CHttpServer::~CHttpServer() = default;
	void CHttpServer::RegisterHandler(HttpMethod, const std::string&, _TyHandler&& handler)
	{
		CHttpRequest request;
		hqtest::State().m_httpResponse = handler(request);
	}
	struct CNetInfo
	{
	};
	CNetPool::CNetPool() = default;
	CNetPool::~CNetPool() = default;
	std::size_t CNetPool::Count() const
	{
		return 123;
	}
	_TyConnectionId CNetPool::CloseAConnection(_TyConnectionId id)
	{
		hqtest::State().m_closed.emplace(id);
		return id;
	}
	bool SendRequest(_TyConnectionId id, const CRequest& req)
	{
		if (hqtest::State().m_failedConnection == id)
		{
			return false;
		}
		hqtest::State().m_sent.emplace_back(id, req);
		return true;
	}
	void SetError(CRequest& req, int code, const std::string& message)
	{
		req.SetErrorInfo(code, message);
	}
	void SetError(CRequest& response, const CRequest& req, int code, const std::string& message)
	{
		response = CRequest();
		response.SetId(req.GetId());
		response.SetType(req.GetType());
		response.SetErrorInfo(code, message);
	}
	void SendError(_TyConnectionId id, const CRequest& req, int code, const std::string& message)
	{
		CRequest response;
		SetError(response, req, code, message);
		SendRequest(id, response);
	}
	std::string CHttpRequest::GetQuery(const std::string& name) const
	{
		const auto mIter = hqtest::State().m_query.find(name);
		return hqtest::State().m_query.end() == mIter ? std::string() : mIter->second;
	}
	CNetEvent::CNetEvent(em_event event) : m_event(event)
	{
	}
	CNetEvent::CNetEvent(em_event event, _TyConnectionId id) : m_event(event), m_connection_id(id)
	{
	}
	CNetEvent::~CNetEvent() = default;
	CNetEvent::CNetEvent(CNetEvent&&) noexcept = default;
	CNetEvent& CNetEvent::operator=(CNetEvent&&) noexcept = default;
} // namespace net

CPythonRuntime::~CPythonRuntime() = default;
bool CPythonRuntime::IsInitialized() const
{
	return true;
}

namespace provider
{
	CMooTdxProvider::~CMooTdxProvider()
	{
		Stop();
	}
	const char* CMooTdxProvider::Name() const
	{
		return "offline-realtime";
	}
	bool CMooTdxProvider::Initialize()
	{
		return true;
	}
	bool CMooTdxProvider::Subscribe(const std::vector<market::CChannelInfo>& infos)
	{
		++hqtest::State().m_subscribeCalls;
		for (const auto& info : infos)
		{
			hqtest::State().m_subscriptions.emplace(info.String());
		}
		return true;
	}
	bool CMooTdxProvider::Unsubscribe(const std::vector<market::CChannelInfo>& infos)
	{
		++hqtest::State().m_unsubscribeCalls;
		for (const auto& info : infos)
		{
			hqtest::State().m_subscriptions.erase(info.String());
		}
		return true;
	}
	std::vector<market::CBar> CMooTdxProvider::QueryBars(const market::CSecurity&, market::Channel, std::int64_t, std::int64_t)
	{
		return {};
	}
	market::CProviderStatus CMooTdxProvider::GetStatus() const
	{
		return { true, "offline" };
	}
	void CMooTdxProvider::SetQuoteHandler(_TyQuoteHandler handler)
	{
		hqtest::State().m_quoteHandler = std::move(handler);
	}
	void CMooTdxProvider::SetDepthHandler(_TyDepthHandler handler)
	{
		hqtest::State().m_depthHandler = std::move(handler);
	}
	void CMooTdxProvider::Stop()
	{
		hqtest::State().m_quoteHandler = {};
		hqtest::State().m_depthHandler = {};
	}
	CAkShareProvider::~CAkShareProvider()
	{
		Stop();
	}
	const char* CAkShareProvider::Name() const
	{
		return "offline-history";
	}
	bool CAkShareProvider::Initialize()
	{
		return true;
	}
	bool CAkShareProvider::Subscribe(const std::vector<market::CChannelInfo>&)
	{
		return false;
	}
	bool CAkShareProvider::Unsubscribe(const std::vector<market::CChannelInfo>&)
	{
		return false;
	}
	std::vector<market::CBar> CAkShareProvider::QueryBars(const market::CSecurity& security, market::Channel channel, std::int64_t, std::int64_t)
	{
		market::CBar bar;
		bar.m_security = security;
		bar.m_channel = channel;
		bar.m_nBeginTime = 1700000000000;
		bar.m_nClosePrice = 123450;
		return { bar };
	}
	market::CProviderStatus CAkShareProvider::GetStatus() const
	{
		return { true, "offline" };
	}
	void CAkShareProvider::SetQuoteHandler(_TyQuoteHandler)
	{
	}
	void CAkShareProvider::SetDepthHandler(_TyDepthHandler)
	{
	}
	void CAkShareProvider::Stop()
	{
	}
	std::vector<market::CInstrument> CAkShareProvider::QueryInstruments() const
	{
		market::CInstrument info;
		info.m_security = { "600519", market::Exchange::sse };
		info.m_strName = "Sample";
		return { info };
	}
	std::vector<market::CSector> CAkShareProvider::QuerySectors(market::SectorType)
	{
		return {};
	}
	market::CSectorConstituents CAkShareProvider::QuerySectorConstituents(market::SectorType, const std::string&)
	{
		return {};
	}
} // namespace provider
