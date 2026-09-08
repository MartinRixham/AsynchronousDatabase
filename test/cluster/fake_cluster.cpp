#include <algorithm>
#include <thread>

#include "cluster/partition.h"
#include "fake_cluster.h"

cluster::fake_cluster::fake_cluster(const std::string &node, const std::vector<std::string> &members):
	self(node)
{
	for (size_t i = 0; i < members.size(); i++)
	{
		member_list.push_back(member { members[i], "" });
	}
}

cluster::fake_cluster::fake_cluster(const std::string &node, const std::vector<member> &members):
	self(node),
	member_list(members)
{
}

void cluster::fake_cluster::owns(const std::string &key, const std::string &node)
{
	owners[key] = std::vector<std::string> { node };
}

void cluster::fake_cluster::copies(const std::string &key, const std::vector<std::string> &nodes)
{
	owners[key] = nodes;
}

void cluster::fake_cluster::answer(const std::string &node, const router::response &response)
{
	answers[node] = response;
}

void cluster::fake_cluster::answer_in_turn(
	const std::string &node,
	const std::vector<router::response> &responses)
{
	answer_list[node] = responses;
}

void cluster::fake_cluster::slow(const std::string &node, std::chrono::milliseconds delay)
{
	delays[node] = delay;
}

std::vector<cluster::member> cluster::fake_cluster::members() const
{
	return member_list;
}

cluster::placement cluster::fake_cluster::replicas(const std::string &key) const
{
	std::map<std::string, std::vector<std::string>>::const_iterator owner = owners.find(key);

	// A key nothing was said about is this node's own, which is what an instance standing alone
	// answers for every key.
	if (owner == owners.end())
	{
		return placement();
	}

	placement where;

	where.local = false;

	for (size_t i = 0; i < owner->second.size(); i++)
	{
		if (owner->second[i] == self)
		{
			where.local = true;
		}
		else
		{
			where.nodes.push_back(owner->second[i]);
		}
	}

	return where;
}

std::vector<std::string> cluster::fake_cluster::peers() const
{
	std::vector<std::string> peers;

	for (size_t i = 0; i < member_list.size(); i++)
	{
		if (member_list[i].node != self)
		{
			peers.push_back(member_list[i].node);
		}
	}

	return peers;
}

void cluster::fake_cluster::led_by(const std::string &key, const std::string &node, int64_t term)
{
	leadership led;

	led.known = true;
	led.local = node == self;
	led.node = node;
	led.term = term;

	leaders[key] = led;
}

void cluster::fake_cluster::led_by_nobody(const std::string &key)
{
	leaders[key] = leadership();
}

void cluster::fake_cluster::applied(const std::string &key, int64_t term)
{
	refused[key] = term;
}

// A key nothing was said about is a cluster with no leadership at all, which is how every test
// that is not about leadership writes.
std::optional<cluster::leadership> cluster::fake_cluster::leader(const std::string &key) const
{
	std::map<std::string, leadership>::const_iterator led = leaders.find(key);

	return led == leaders.end() ? std::optional<leadership>() : led->second;
}

size_t cluster::fake_cluster::leads() const
{
	return std::count_if(
		leaders.begin(),
		leaders.end(),
		[](const std::pair<std::string, leadership> &led) { return led.second.local; });
}

bool cluster::fake_cluster::accept(const std::string &key, int64_t term)
{
	std::map<std::string, int64_t>::const_iterator seen = refused.find(key);

	return term == 0 || seen == refused.end() || term >= seen->second;
}

std::vector<std::vector<std::string>> cluster::fake_cluster::zones() const
{
	std::string zone;

	for (size_t i = 0; i < member_list.size(); i++)
	{
		if (member_list[i].node == self)
		{
			zone = member_list[i].zone;
		}
	}

	return zones_of(member_list, self, zone);
}

router::response cluster::fake_cluster::send(const std::string &node, const router::request &request) const
{
	requests.push_back(std::pair<std::string, router::request>(node, request));

	std::map<std::string, std::chrono::milliseconds>::const_iterator waits = delays.find(node);

	if (waits != delays.end())
	{
		std::this_thread::sleep_for(waits->second);
	}

	std::map<std::string, std::vector<router::response>>::const_iterator in_turn = answer_list.find(node);

	if (in_turn != answer_list.end() && !in_turn->second.empty())
	{
		size_t given = answered[node];

		answered[node] = given + 1;

		return in_turn->second[std::min(given, in_turn->second.size() - 1)];
	}

	std::map<std::string, router::response>::const_iterator answered_once = answers.find(node);

	if (answered_once == answers.end())
	{
		return router::empty_response(boost::beast::http::status::no_content);
	}

	return answered_once->second;
}

const std::vector<std::pair<std::string, router::request>> &cluster::fake_cluster::sent() const
{
	return requests;
}

void cluster::fake_cluster::forget()
{
	requests.clear();
}
