#ifndef __MAPPED_VALUE_H__
#define __MAPPED_VALUE_H__

namespace container
{
	template <class _TyContainer, class _TyKey>
	const typename _TyContainer::mapped_type& FindMappedValue(const _TyContainer& container, const _TyKey& key)
	{
		typename _TyContainer::const_iterator iter = container.find(key);
		if (iter != container.end())
		{
			return iter->second;
		}

		thread_local typename _TyContainer::mapped_type emptyValue;
		return emptyValue;
	}
} // namespace container

#endif
