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

	size_t stringsplit(const std::string& s, std::vector<std::string>& vc, char delim, bool bEmpty)
	{
		vc.clear();
		const char* p = s.c_str();
		size_t n = strlen(p);
		size_t opos = 0;
		for (size_t i = 0; i < n; ++i)
		{
			bool bDelim = (*(p + i) == delim);
			bool bEnd = i >= (n - 1);
			if (bDelim)
			{
				if ((i - opos > 0) || bEmpty)
				{
					vc.emplace_back(s.substr(opos, i - opos));
				}
				opos = bEnd ? i : i + 1;
			}
			if (bEnd)
			{
				if (bDelim)
				{
					if (bEmpty)
					{
						vc.emplace_back(s.substr(opos, 0));
					}
				}
				else
				{
					vc.emplace_back(s.substr(opos, -1));
				}
			}
		}
		return vc.size();
	}

} // namespace utility
