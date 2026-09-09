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
	for (size_t i = 0; i < response.headers.size(); i++)
	{
		if (same_name(response.headers[i].first, name))
		{
			return response.headers[i].second;
		}
	}

	return "";
}
