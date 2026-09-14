#ifndef __CDISTRIBUTOR_H__
#define __CDISTRIBUTOR_H__
#include <algorithm>
#include <chrono>
#include <shared_mutex>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include "../common/defines.h"
#include "../common/utility.h"

template <bool bAsyn, class _Ty, class _TyHandler>
class CDistributor
{
		using _TyData = Typer<_Ty>::type;
		using _TyDataContainer = std::vector<_TyData>;

	public:
		CDistributor() = default;
		~CDistributor()
		{
			StopAndWait();
		}

	public:
		void Dispatch(_Ty&& data)
		{
			if constexpr (bAsyn)
			{
				std::lock_guard<std::mutex> taskLock(m_mtx_tasks);
				if (m_stopping)
				{
					return;
				}
				RemoveCompletedTasks();
				if constexpr (IsContainer<_Ty>)
				{
					{
						std::unique_lock<std::shared_mutex> lock(m_smtx_data);
						m_cache.reserve(m_cache.size() + data.size());
						m_cache.insert(m_cache.end(), std::make_move_iterator(data.begin()),
									   std::make_move_iterator(data.end()));
					}
				}
				else
				{
					std::unique_lock<std::shared_mutex> lock(m_smtx_data);
					m_cache.emplace_back(std::move(data));
				}
				m_tasks.emplace_back(ThreadPoolPtr->PushTask(task_priority::em_normal, 0, [this]()
										{
											return AsyncExecute();
										}));
			}
			else
			{
				Execute(std::forward<_Ty>(data));
			}
		}

		void RegisterHandler(_TyHandler&& fun)
		{
			std::unique_lock<std::shared_mutex> lock(m_smtx_handler);
			m_handler.emplace_back(std::forward<_TyHandler>(fun));
		}

		void StopAndWait()
		{
			// 等待所有任务结束，避免异步任务捕获的资源在断线释放后失效。
			if constexpr (bAsyn)
			{
				std::vector<std::future<int>> tasks;
				{
					std::lock_guard<std::mutex> lock(m_mtx_tasks);
					m_stopping = true;
					tasks.swap(m_tasks);
				}
				for (auto& task : tasks)
				{
					task.wait();
				}
			}
		}

	private:
		void RemoveCompletedTasks()
		{
			m_tasks.erase(std::remove_if(m_tasks.begin(), m_tasks.end(), [](std::future<int>& task)
			{
				return std::future_status::ready == task.wait_for(std::chrono::seconds(0));
			}), m_tasks.end());
		}

		int AsyncExecute()
		{
			_TyDataContainer data;
			{
				std::unique_lock<std::shared_mutex> lock(m_smtx_data);
				data.swap(m_cache);
			}

			return Execute(std::move(data));
		}

		int Execute(_Ty&& data)
		{
			int ret = 1;
			if constexpr (IsContainer<_Ty>)
			{
				std::shared_lock<std::shared_mutex> lock(m_smtx_handler);
				std::size_t sz = data.size();
				for (std::size_t i = 0; i < sz; ++i)
				{
					if (ExecuteA(data.at(i)) == 0)
					{
						ret = 0;
					}
				}
			}
			else
			{
				ret = ExecuteA(data);
			}
			return ret;
		}

		int ExecuteA(const _TyData& data)
		{
			std::size_t sz = m_handler.size();
			int ret = 1;
			for (std::size_t i = 0; i < sz; ++i)
			{
				const _TyHandler& fun = m_handler.at(i);
				if ((nullptr == fun) || !fun(data))
				{
					ret = 0;
				}
			}
			return ret;
		}

	private:
		std::shared_mutex m_smtx_data;
		_TyDataContainer m_cache;

		std::shared_mutex m_smtx_handler;
		std::vector<_TyHandler> m_handler;

		std::mutex m_mtx_tasks;
		std::vector<std::future<int>> m_tasks;
		bool m_stopping{ false };
};

#endif
