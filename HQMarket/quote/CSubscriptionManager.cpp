#include "CSubscriptionManager.h"
namespace market
{
	std::string CSubscriptionManager::MakeKey(const CSubscription& s)
	{
		return s.m_instrument.Key() + ":" + std::to_string(static_cast<int>(s.m_channel));
	}
	std::vector<CSubscription> CSubscriptionManager::Subscribe(net::_TyConnectionId nClientId,
															   const std::vector<CSubscription>& subscriptions)
	{
		std::lock_guard<std::mutex> lck(m_mtx_subscriptions);
		std::vector<CSubscription> added;
		auto& client = m_clients[nClientId];
		for (const auto& s : subscriptions)
		{
			std::string key = MakeKey(s);
			if (!client.emplace(key, s).second)
			{
				continue;
			}
			if (1 == ++m_refCounts[key])
			{
				added.emplace_back(s);
			}
		}
		return added;
	}
	std::vector<CSubscription> CSubscriptionManager::Unsubscribe(net::_TyConnectionId nClientId,
																 const std::vector<CSubscription>& subscriptions)
	{
		std::lock_guard<std::mutex> lck(m_mtx_subscriptions);
		std::vector<CSubscription> removed;
		std::unordered_map<net::_TyConnectionId, std::unordered_map<std::string, CSubscription>>::iterator client =
			m_clients.find(nClientId);
		if (m_clients.end() == client)
		{
			return removed;
		}
		for (const auto& s : subscriptions)
		{
			std::string key = MakeKey(s);
			if (0 == client->second.erase(key))
			{
				continue;
			}
			std::unordered_map<std::string, std::size_t>::iterator ref = m_refCounts.find(key);
			if ((m_refCounts.end() != ref) && (0 == --ref->second))
			{
				m_refCounts.erase(ref);
				removed.emplace_back(s);
			}
		}
		if (client->second.empty())
		{
			m_clients.erase(client);
		}
		return removed;
	}
	std::vector<CSubscription> CSubscriptionManager::RemoveClient(net::_TyConnectionId nClientId)
	{
		std::lock_guard<std::mutex> lck(m_mtx_subscriptions);
		std::vector<CSubscription> removed;
		std::unordered_map<net::_TyConnectionId, std::unordered_map<std::string, CSubscription>>::iterator client =
			m_clients.find(nClientId);
		if (m_clients.end() == client)
		{
			return removed;
		}
		for (const auto& [key, s] : client->second)
		{
			std::unordered_map<std::string, std::size_t>::iterator ref = m_refCounts.find(key);
			if ((m_refCounts.end() != ref) && (0 == --ref->second))
			{
				m_refCounts.erase(ref);
				removed.emplace_back(s);
			}
		}
		m_clients.erase(client);
		return removed;
	}
	std::size_t CSubscriptionManager::SubscriptionCount() const
	{
		std::lock_guard<std::mutex> lck(m_mtx_subscriptions);
		return m_refCounts.size();
	}
	bool CSubscriptionManager::IsSubscribed(net::_TyConnectionId nClientId, const CSubscription& subscription) const
	{
		std::lock_guard<std::mutex> lck(m_mtx_subscriptions);
		std::unordered_map<net::_TyConnectionId, std::unordered_map<std::string, CSubscription>>::const_iterator client =
			m_clients.find(nClientId);
		return (m_clients.end() != client) && client->second.contains(MakeKey(subscription));
	}
} // namespace market
