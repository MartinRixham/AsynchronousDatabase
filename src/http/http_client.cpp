#include <algorithm>
#include <cctype>

#include "http_client.h"

namespace
{
	bool same_name(const std::string &one, const std::string &other)
	{
		return one.size() == other.size() &&
			std::equal(
				one.begin(),
				one.end(),
				other.begin(),
				[](char left, char right)
				{
					return std::tolower(static_cast<unsigned char>(left)) ==
						std::tolower(static_cast<unsigned char>(right));
				});
	}
}

std::string http::header_of(const response &response, const std::string &name)
{
	for (const auto &[held, value] : response.headers)
	{
		if (same_name(held, name))
		{
			return value;
		}
	}

	return "";
}
