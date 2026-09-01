#include "CSubscriptionMgr.h"
#include <utility>

namespace market
{
	std::vector<CChannelInfo> CSubscriptionMgr::Subscribe(net::_TyConnectionId id, const std::vector<CChannelInfo>& channels)
	{
		std::vector<CChannelInfo> adds;
		adds.reserve(channels.size());

		std::lock_guard<std::shared_mutex> lock(m_mtx_info);
		_TyClientSubscriptions& infos = m_id_infos[id];
		for (const auto& info : channels)
		{
			std::string key = info.String();
			if (!infos.emplace(key, info).second)
			{
				continue;
			}
			_TySubscriberIds& ids = m_info_ids[key];
			ids.emplace(id);
			if (1 == ids.size())
			{
				adds.emplace_back(info);
			}
		}
		return adds;
	}

	std::vector<CChannelInfo> CSubscriptionMgr::Unsubscribe(net::_TyConnectionId id, const std::vector<CChannelInfo>& channels)
	{
		std::vector<CChannelInfo> removed;
		removed.reserve(channels.size());

		std::lock_guard<std::shared_mutex> lock(m_mtx_info);
		auto mIter = m_id_infos.find(id);
		if (m_id_infos.end() == mIter)
		{
			return removed;
		}
		for (const auto& info : channels)
		{
			std::string key = info.String();
			auto mmIter = mIter->second.find(key);
			if (mIter->second.end() == mmIter)
			{
				continue;
			}
			auto idsIter = m_info_ids.find(key);
			if (m_info_ids.end() != idsIter)
			{
				idsIter->second.erase(id);
				if (idsIter->second.empty())
				{
					removed.emplace_back(mmIter->second);
					m_info_ids.erase(idsIter);
				}
			}
			mIter->second.erase(mmIter);
		}
		if (mIter->second.empty())
		{
			m_id_infos.erase(mIter);
		}
		return removed;
	}

	std::vector<CChannelInfo> CSubscriptionMgr::RemoveClient(net::_TyConnectionId id)
	{
		std::vector<CChannelInfo> removed;

		std::lock_guard<std::shared_mutex> lock(m_mtx_info);
		auto mIter = m_id_infos.find(id);
		if (m_id_infos.end() == mIter)
		{
			return removed;
		}
		removed.reserve(mIter->second.size());
		for (const auto& info : mIter->second)
		{
			auto mmIter = m_info_ids.find(info.first);
			if (m_info_ids.end() == mmIter)
			{
				continue;
			}
			mmIter->second.erase(id);
			if (mmIter->second.empty())
			{
				removed.emplace_back(info.second);
				m_info_ids.erase(mmIter);
			}
		}
		m_id_infos.erase(mIter);
		return removed;
	}

	std::size_t CSubscriptionMgr::GetSubscriptionCount(net::_TyConnectionId id) const
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_info);
		const auto mIter = m_id_infos.find(id);
		return m_id_infos.end() == mIter ? 0 : mIter->second.size();
	}

	std::size_t CSubscriptionMgr::GetSubscriptionCount(const CChannelInfo& info) const
	{
		std::string key = info.String();
		if (key.empty())
		{
			return 0;
		}
		std::shared_lock<std::shared_mutex> lock(m_mtx_info);
		const auto mIter = m_info_ids.find(key);
		return m_info_ids.end() == mIter ? 0 : mIter->second.size();
	}

	bool CSubscriptionMgr::IsSubscribed(net::_TyConnectionId id, const CChannelInfo& info) const
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_info);
		const auto mIter = m_id_infos.find(id);
		return (m_id_infos.end() != mIter) && mIter->second.contains(info.String());
	}
} // namespace market
