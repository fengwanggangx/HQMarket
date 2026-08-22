#ifndef __CSUBSCRIPTION_MANAGER_H__
#define __CSUBSCRIPTION_MANAGER_H__
#include "MarketTypes.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
namespace market
{
	class CSubscriptionManager final
	{
		public:
			std::vector<CSubscription> Subscribe(std::uint64_t nClientId, const std::vector<CSubscription>& subscriptions);
			std::vector<CSubscription> Unsubscribe(std::uint64_t nClientId, const std::vector<CSubscription>& subscriptions);
			std::vector<CSubscription> RemoveClient(std::uint64_t nClientId);
			std::size_t SubscriptionCount() const;
			bool IsSubscribed(std::uint64_t nClientId, const CSubscription& subscription) const;

		private:
			static std::string MakeKey(const CSubscription& subscription);
			mutable std::mutex m_mtxSubscriptions;
			std::unordered_map<std::string, std::size_t> m_refCounts;
			std::unordered_map<std::uint64_t, std::unordered_map<std::string, CSubscription>> m_clients;
	};
} // namespace market
#endif
