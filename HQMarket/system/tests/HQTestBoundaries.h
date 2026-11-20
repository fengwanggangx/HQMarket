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
		std::unique_ptr<net::CHttpResponseData> m_http_response;
		market::IMarketProvider::_TyQuoteHandler m_quote_handler;
		market::IMarketProvider::_TyDepthHandler m_depth_handler;
		net::_TyConnectionId m_failed_connection{ -1 };
		std::size_t m_subscribe_calls{ 0 };
		std::size_t m_unsubscribe_calls{ 0 };
	};

	CBoundaryState& State();
} // namespace hqtest
