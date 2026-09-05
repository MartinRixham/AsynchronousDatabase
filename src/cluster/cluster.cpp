#include <algorithm>

#include "cluster.h"

// The cluster that cannot ask several nodes at once asks them one after another, which is what a
// cluster standing alone and a cluster a test names itself both do. It answers the same as one
// that asked them at once, down to which refusal it reports: every node is asked whatever the one
// before it said, and the answer is the first refusal in the order they were asked.
std::optional<router::response> cluster::cluster::send_all(
	const std::vector<std::string> &node_list,
	const router::request &request) const
{
	std::vector<router::response> answers;

	for (size_t i = 0; i < node_list.size(); i++)
	{
		answers.push_back(send(node_list[i], request));
	}

	return refusal(answers);
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
