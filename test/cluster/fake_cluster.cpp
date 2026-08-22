#include "fake_cluster.h"

cluster::fake_cluster::fake_cluster(const std::string &node, const std::vector<std::string> &members):
	self(node),
	member_list(members)
{
}

void cluster::fake_cluster::owns(const std::string &key, const std::string &node)
{
	owners[key] = node;
}

void cluster::fake_cluster::answer(const std::string &node, const router::response &response)
{
	answers[node] = response;
}

std::vector<std::string> cluster::fake_cluster::members() const
{
	return member_list;
}

std::optional<std::string> cluster::fake_cluster::owner(const std::string &key) const
{
	std::map<std::string, std::string>::const_iterator owner = owners.find(key);

	// A key nothing was said about is this node's own, which is what an instance standing alone
	// answers for every key.
	if (owner == owners.end() || owner->second == self)
	{
		return std::nullopt;
	}

	return owner->second;
}

std::vector<std::string> cluster::fake_cluster::peers() const
{
	std::vector<std::string> peers;

	for (size_t i = 0; i < member_list.size(); i++)
	{
		if (member_list[i] != self)
		{
			peers.push_back(member_list[i]);
		}
	}

	return peers;
}

router::response cluster::fake_cluster::send(const std::string &node, const router::request &request) const
{
	requests.push_back(std::pair<std::string, router::request>(node, request));

	std::map<std::string, router::response>::const_iterator answered = answers.find(node);

	if (answered == answers.end())
	{
		return router::empty_response(boost::beast::http::status::no_content);
	}

	return answered->second;
}

const std::vector<std::pair<std::string, router::request>> &cluster::fake_cluster::sent() const
{
	return requests;
}

void cluster::fake_cluster::forget()
{
	requests.clear();
}
