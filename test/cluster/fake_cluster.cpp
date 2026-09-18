#include <algorithm>
#include <iterator>

#include "cluster/partition.h"
#include "fake_cluster.h"

cluster::fake_cluster::fake_cluster(const std::string &node, const std::vector<std::string> &members):
	self(node),
	mutex(std::make_shared<std::mutex>())
{
	std::ranges::transform(
		members,
		std::back_inserter(member_list),
		[](const std::string &name) { return member { name, "" }; });

	filled.set();
}

cluster::fake_cluster::fake_cluster(const std::string &node, const std::vector<member> &members):
	self(node),
	member_list(members),
	mutex(std::make_shared<std::mutex>())
{
	filled.set();
}

void cluster::fake_cluster::owns(const std::string &key, const std::string &node)
{
	owners[key] = std::vector<std::string> { node };
}

void cluster::fake_cluster::copies(const std::string &key, const std::vector<std::string> &nodes)
{
	owners[key] = nodes;
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
	auto owner = owners.find(key);

	// A key nothing was said about is this node's own, which is what an instance standing alone
	// answers for every key.
	return owner == owners.end() ? placement() : placed(owner->second);
}

// A test names the keys a node holds rather than the partitions, so a partition is held by
// whatever was said about a key that is in it. Nothing was said about the rest, which is this
// node's own — the same answer replicas() gives for such a key.
cluster::placement cluster::fake_cluster::copies_of(size_t partition) const
{
	for (const auto &[key, nodes] : owners)
	{
		if (partition_of(key) == partition)
		{
			return placed(nodes);
		}
	}

	return placement();
}

cluster::placement cluster::fake_cluster::placed(const std::vector<std::string> &nodes) const
{
	placement where;

	where.local = false;

	for (const auto &node : nodes)
	{
		if (node == self)
		{
			where.local = true;
		}
		else
		{
			where.nodes.push_back(node);
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

	for (const auto &[key, nodes] : owners)
	{
		if (std::find(nodes.begin(), nodes.end(), self) == nodes.end())
		{
			held.reset(partition_of(key));
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

	for (const auto &node : nodes)
	{
		asking[node] = partitions;
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

	for (const auto &node : zone)
	{
		asking[node] = partitions;
	}

	return asking;
}

std::vector<std::string> cluster::fake_cluster::peers() const
{
	std::vector<std::string> peers;

	for (const member &listed : member_list)
	{
		if (listed.node != self)
		{
			peers.push_back(listed.node);
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
std::optional<cluster::leadership> cluster::fake_cluster::leader(const std::string &key)
{
	auto led = leaders.find(key);

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

void cluster::fake_cluster::alone()
{
	alone_node = true;
}

bool cluster::fake_cluster::is_alone() const
{
	return alone_node;
}

void cluster::fake_cluster::unvouched()
{
	std::lock_guard<std::mutex> lock(*mutex);

	filled.reset();
}

void cluster::fake_cluster::unvouched(const std::string &key)
{
	std::lock_guard<std::mutex> lock(*mutex);

	filled.reset(partition_of(key));
}

uint64_t cluster::fake_cluster::generation() const
{
	return 0;
}

void cluster::fake_cluster::vouch(const partition_set &partitions, uint64_t)
{
	std::lock_guard<std::mutex> lock(*mutex);

	filled |= partitions;
}

// A partition this node does not hold is one it has nothing to vouch for, as in etcd_cluster.
cluster::partition_set cluster::fake_cluster::vouched() const
{
	std::lock_guard<std::mutex> lock(*mutex);

	return filled & holdings();
}

void cluster::fake_cluster::reads_etcd(const std::string &endpoint)
{
	std::lock_guard<std::mutex> lock(*mutex);

	etcd_state.configured = true;
	etcd_state.held = true;
	etcd_state.endpoint = endpoint;
	etcd_state.registrations = 1;
}

void cluster::fake_cluster::lost_etcd()
{
	std::lock_guard<std::mutex> lock(*mutex);

	etcd_state.held = false;
}

// A server reads the registration from a thread of its own while the test changes it.
void cluster::fake_cluster::registered_again()
{
	std::lock_guard<std::mutex> lock(*mutex);

	etcd_state.held = true;
	etcd_state.registrations++;
}

cluster::etcd_registration cluster::fake_cluster::registration() const
{
	std::lock_guard<std::mutex> lock(*mutex);

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
	auto seen = refused.find(key);

	if (term == 0)
	{
		return true;
	}

	return term >= restored[partition_of(key)] && (seen == refused.end() || term >= seen->second);
}

void cluster::fake_cluster::restore_terms(const partition_terms &terms)
{
	restored = terms;
}

std::vector<std::vector<std::string>> cluster::fake_cluster::zones() const
{
	std::string zone;

	for (const member &listed : member_list)
	{
		if (listed.node == self)
		{
			zone = listed.zone;
		}
	}

	return zones_of(member_list, self, zone);
}
