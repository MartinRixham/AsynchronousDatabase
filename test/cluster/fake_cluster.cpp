#include <algorithm>
#include <thread>

#include "cluster/partition.h"
#include "fake_cluster.h"

cluster::fake_cluster::fake_cluster(const std::string &node, const std::vector<std::string> &members):
	self(node),
	mutex(std::make_shared<std::mutex>())
{
	for (size_t i = 0; i < members.size(); i++)
	{
		member_list.push_back(member { members[i], "" });
	}
}

cluster::fake_cluster::fake_cluster(const std::string &node, const std::vector<member> &members):
	self(node),
	member_list(members),
	mutex(std::make_shared<std::mutex>())
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

void cluster::fake_cluster::start()
{
}

bool cluster::fake_cluster::discover()
{
	return false;
}

void cluster::fake_cluster::stop()
{
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
	return owner == owners.end() ? placement() : placed(owner->second);
}

// A test names the keys a node holds rather than the partitions, so a partition is held by
// whatever was said about a key that is in it. Nothing was said about the rest, which is this
// node's own — the same answer replicas() gives for such a key.
cluster::placement cluster::fake_cluster::copies_of(size_t partition) const
{
	for (std::map<std::string, std::vector<std::string>>::const_iterator it = owners.begin();
		it != owners.end();
		++it)
	{
		if (partition_of(it->first) == partition)
		{
			return placed(it->second);
		}
	}

	return placement();
}

cluster::placement cluster::fake_cluster::placed(const std::vector<std::string> &nodes) const
{
	placement where;

	where.local = false;

	for (size_t i = 0; i < nodes.size(); i++)
	{
		if (nodes[i] == self)
		{
			where.local = true;
		}
		else
		{
			where.nodes.push_back(nodes[i]);
		}
	}

	return where;
}

// Every partition, less the ones holding a key the test said belongs somewhere else. It is
// replicas() asked of all of them at once, and a key nothing was said about is this node's own
// there too.
cluster::partition_set cluster::fake_cluster::holdings() const
{
	partition_set held;

	held.set();

	for (std::map<std::string, std::vector<std::string>>::const_iterator it = owners.begin();
		it != owners.end();
		++it)
	{
		if (std::find(it->second.begin(), it->second.end(), self) == it->second.end())
		{
			held.reset(partition_of(it->first));
		}
	}

	return held;
}

// Every peer, because which of them holds a partition is what the hashing answers rather than
// something a test hands in: what a test says here is what each node answers when it is asked.
// Which nodes a share names is holders_of()'s to say, and partition_test's to prove.
std::map<std::string, cluster::partition_set> cluster::fake_cluster::holders(
	const partition_set &partitions) const
{
	std::map<std::string, partition_set> asking;
	std::vector<std::string> nodes = peers();

	for (size_t i = 0; i < nodes.size(); i++)
	{
		asking[nodes[i]] = partitions;
	}

	return asking;
}

// Every node of the zone, for the same reason: a fake cluster hashes nothing, so what a test says
// here is what each node answers when it is asked for a share.
std::map<std::string, cluster::partition_set> cluster::fake_cluster::holders_in(
	const partition_set &partitions,
	const std::vector<std::string> &zone) const
{
	std::map<std::string, partition_set> asking;

	for (size_t i = 0; i < zone.size(); i++)
	{
		asking[zone[i]] = partitions;
	}

	return asking;
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

void cluster::fake_cluster::unled()
{
	unled_node = true;
}

bool cluster::fake_cluster::is_unled() const
{
	return unled_node;
}

void cluster::fake_cluster::reads_etcd(const std::string &endpoint)
{
	etcd_state.configured = true;
	etcd_state.held = true;
	etcd_state.endpoint = endpoint;
}

void cluster::fake_cluster::lost_etcd()
{
	etcd_state.held = false;
}

cluster::etcd_registration cluster::fake_cluster::registration() const
{
	return etcd_state;
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

// The answer is settled under the lock and the wait is not, so a node that takes a while to answer
// takes a while to answer every caller rather than holding the others out of the cluster.
router::response cluster::fake_cluster::send(const std::string &node, const router::request &request) const
{
	std::chrono::milliseconds waiting(0);
	router::response given = router::empty_response(boost::beast::http::status::no_content);

	{
		std::lock_guard<std::mutex> lock(*mutex);

		requests.push_back(std::pair<std::string, router::request>(node, request));

		std::map<std::string, std::chrono::milliseconds>::const_iterator waits = delays.find(node);

		if (waits != delays.end())
		{
			waiting = waits->second;
		}

		std::map<std::string, std::vector<router::response>>::const_iterator in_turn = answer_list.find(node);

		if (in_turn != answer_list.end() && !in_turn->second.empty())
		{
			size_t taken = answered[node];

			answered[node] = taken + 1;

			given = in_turn->second[std::min(taken, in_turn->second.size() - 1)];
		}
		else
		{
			std::map<std::string, router::response>::const_iterator answered_once = answers.find(node);

			if (answered_once != answers.end())
			{
				given = answered_once->second;
			}
		}
	}

	if (waiting.count() > 0)
	{
		std::this_thread::sleep_for(waiting);
	}

	return given;
}

// A fake with nothing to ask at once asks them one after another.
std::optional<router::response> cluster::fake_cluster::send_all(
	const std::vector<std::string> &node_list,
	const router::request &request) const
{
	std::vector<router::response> responses;

	for (size_t i = 0; i < node_list.size(); i++)
	{
		responses.push_back(send(node_list[i], request));
	}

	return refusal(responses);
}

// One after another rather than at once. What a fan out is for is the time it saves, and there is
// none of that to save here.
std::vector<router::response> cluster::fake_cluster::send_each(const std::vector<enquiry> &enquiries) const
{
	std::vector<router::response> responses;

	for (size_t i = 0; i < enquiries.size(); i++)
	{
		responses.push_back(send(enquiries[i].node, enquiries[i].request));
	}

	return responses;
}

const std::vector<std::pair<std::string, router::request>> &cluster::fake_cluster::sent() const
{
	return requests;
}

void cluster::fake_cluster::forget()
{
	requests.clear();
}
