#ifndef __CTHREADPOOL_DEFINES_H__
#define __CTHREADPOOL_DEFINES_H__

enum class task_priority
{
	em_normal = 0,	//一般任务
	em_middle,		//居中
	em_high			//高优先级任务
};

enum class pool_type
{
	em_more_calc = 0,	//CPU密集型，大量数据处理计算
	em_more_io,			//IO密集型，IO等待
	em_more_complex		//混合任务型
};

enum class abort_policy
{
	em_callerrunspolicy = 0,	//调用者线程执行任务
	em_discardoldestpolicy,		//丢弃原队列中队尾任务，换成新的
	em_discardpolicy			//丢弃任务
};

class Task
{
public:
	friend struct TaskComparator;
	friend class CThreadPool;

public:
	Task() = default;
	template<typename _Fx>
	Task(task_priority level, size_t priority, _Fx&& func) noexcept : m_level(level), m_priority(priority)
	{
		m_task = [func = std::forward<_Fx>(func)]() mutable { (*func)(); };
	}
	~Task()
	{
		m_task = nullptr;
	}

private:
	task_priority	m_level{ task_priority::em_normal };
	size_t	m_priority{ 0 };
	std::function<void()> m_task{ nullptr };
};

struct TaskComparator
{
	//大端:优先级高，先出栈
	bool operator()(const Task& p1, const Task& p2)
	{
		return (int(p1.m_level) == int(p2.m_level)) ? (p1.m_priority < p2.m_priority) : (int(p1.m_level) < int(p2.m_level));
	}
};

#endif