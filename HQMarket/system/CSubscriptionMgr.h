#ifndef HQMARKET_CSUBSCRIPTION_MGR_H
#define HQMARKET_CSUBSCRIPTION_MGR_H

#include "../quote/MarketTypes.h"
#include "../network/CNet.h"
#include "../network/common_net.h"
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CSubscriptionMgr final
{
	public:
		std::vector<market::CChannelInfo> Subscribe(net::_TyConnectionId id, const std::vector<market::CChannelInfo>& channels);
		std::vector<market::CChannelInfo> Unsubscribe(net::_TyConnectionId id, const std::vector<market::CChannelInfo>& channels);
		std::vector<market::CChannelInfo> RemoveClient(net::_TyConnectionId id);

		std::size_t GetSubscriptionCount(net::_TyConnectionId id) const;
		std::size_t GetSubscriptionCount(const market::CChannelInfo& info) const;
		bool IsSubscribed(net::_TyConnectionId id, const market::CChannelInfo& info) const;

	private:
		using _TyClientSubscriptions = std::unordered_map<std::string, market::CChannelInfo>;
		using _TySubscriberIds = std::unordered_set<net::_TyConnectionId>;

		mutable std::shared_mutex m_mtx_info;
		std::unordered_map<std::string, _TySubscriberIds> m_info_ids;
		std::unordered_map<net::_TyConnectionId, _TyClientSubscriptions> m_id_infos;
};

#endif
