#include "CThreadPool.h"
#include <algorithm>
CThreadPool::CThreadPool(std::size_t nThreads)
{
	if (nThreads == 0) nThreads = std::max<std::size_t>(2, std::thread::hardware_concurrency());
	for (std::size_t i = 0; i < nThreads; ++i) m_threads.emplace_back(&CThreadPool::Run, this);
}
CThreadPool::~CThreadPool() { Stop(); }
void CThreadPool::PushTask(task_priority, int, std::function<void()>&& task)
{
	{ std::lock_guard lock(m_mtxTasks); if (m_bStopping) return; m_tasks.emplace(std::move(task)); }
	m_cvTasks.notify_one();
}
void CThreadPool::Stop()
{
	{ std::lock_guard lock(m_mtxTasks); if (m_bStopping) return; m_bStopping = true; }
	m_cvTasks.notify_all(); for (auto& thread : m_threads) if (thread.joinable()) thread.join(); m_threads.clear();
}
void CThreadPool::Run()
{
	while (true) { std::function<void()> task; { std::unique_lock lock(m_mtxTasks); m_cvTasks.wait(lock, [this]() { return m_bStopping || !m_tasks.empty(); }); if (m_bStopping && m_tasks.empty()) return; task = std::move(m_tasks.front()); m_tasks.pop(); } task(); }
}
