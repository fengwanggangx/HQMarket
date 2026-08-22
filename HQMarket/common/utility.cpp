#include "utility.h"

#include <algorithm>
#include <cctype>

namespace utility
{
	std::string lower(std::string strVal)
	{
		std::transform(strVal.begin(), strVal.end(), strVal.begin(), [](unsigned char ch)
		{
			return static_cast<char>(std::tolower(ch));
		});
		return strVal;
	}
} // namespace utility
