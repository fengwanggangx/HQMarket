#include "defines.h"
CThreadPool* GetThreadPool()
{
	static CThreadPool* pInstance = new CThreadPool(pool_type::em_more_calc);
	return pInstance;
}
