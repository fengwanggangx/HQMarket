#pragma once
#include "../../request/request.h"
#include "../../network/CHttpServer.h"
#include "../../quote/IMarketProvider.h"
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hqtest
{
	struct CBoundaryState
	{
		std::vector<std::pair<net::_TyConnectionId, CRequest>> m_sent;
		std::unordered_set<net::_TyConnectionId> m_closed;
		std::unordered_set<std::string> m_subscriptions;
		std::unordered_map<std::string, std::string> m_query;
		std::unique_ptr<net::CHttpResponseData> m_httpResponse;
		market::IMarketProvider::_TyQuoteHandler m_quoteHandler;
		market::IMarketProvider::_TyDepthHandler m_depthHandler;
		net::_TyConnectionId m_failedConnection{ -1 };
		std::size_t m_subscribeCalls{ 0 };
		std::size_t m_unsubscribeCalls{ 0 };
	};

	CBoundaryState& State();
} // namespace hqtest
