#include <algorithm>

#include "cluster.h"

std::optional<router::response> cluster::refusal(const std::vector<router::response> &answers)
{
	std::vector<router::response>::const_iterator refused = std::find_if(
		answers.begin(),
		answers.end(),
		[](const router::response &answer)
		{
			return answer.status >= boost::beast::http::status::bad_request;
		});

	if (refused == answers.end())
	{
		return std::nullopt;
	}

	return *refused;
}
