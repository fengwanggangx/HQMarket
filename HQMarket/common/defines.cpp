#include "defines.h"
CThreadPool* GetThreadPool()
{
	static CThreadPool s_pool;
	return &s_pool;
}
