#ifndef __CSUBSCRIPTION_MANAGER_H__
#define __CSUBSCRIPTION_MANAGER_H__
#include "MarketTypes.h"
#include "../network/CNet.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
namespace market
{
	class CSubscriptionManager final
	{
		public:
			std::vector<CSubscription> Subscribe(net::_TyConnectionId nClientId, const std::vector<CSubscription>& subscriptions);
			std::vector<CSubscription> Unsubscribe(net::_TyConnectionId nClientId, const std::vector<CSubscription>& subscriptions);
			std::vector<CSubscription> RemoveClient(net::_TyConnectionId nClientId);
			std::size_t SubscriptionCount() const;
			bool IsSubscribed(net::_TyConnectionId nClientId, const CSubscription& subscription) const;

		private:
			static std::string MakeKey(const CSubscription& subscription);
			mutable std::mutex m_mtx_subscriptions;
			std::unordered_map<std::string, std::size_t> m_refCounts;
			std::unordered_map<net::_TyConnectionId, std::unordered_map<std::string, CSubscription>> m_clients;
	};
} // namespace market
#endif
