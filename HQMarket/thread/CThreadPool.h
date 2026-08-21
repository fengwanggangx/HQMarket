#ifndef __CTHREAD_POOL_H__
#define __CTHREAD_POOL_H__
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
enum class task_priority { em_low = 0, em_normal = 1, em_high = 2 };
class CThreadPool final
{
public:
	explicit CThreadPool(std::size_t nThreads = 0);
	~CThreadPool();
	void PushTask(task_priority, int, std::function<void()>&& task);
	void Stop();
private:
	void Run();
	std::mutex m_mtxTasks;
	std::condition_variable m_cvTasks;
	std::queue<std::function<void()>> m_tasks;
	std::vector<std::thread> m_threads;
	bool m_bStopping{ false };
};
#endif
