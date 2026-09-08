#include <algorithm>

#include "cluster.h"

cluster::placements::placements(const cluster &nodes):
	nodes(nodes)
{
}

const cluster::placement &cluster::placements::of(const std::string &key)
{
	std::optional<placement> &answer = known[partition_of(key)];

	if (!answer)
	{
		answer = nodes.replicas(key);
	}

	return *answer;
}

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
