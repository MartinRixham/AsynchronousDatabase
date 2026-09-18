#include <algorithm>
#include <mutex>
#include <shared_mutex>

#include "cluster/partition.h"
#include "test_cluster.h"

cluster::test_cluster::test_cluster()
{
	filled.set();
}

void cluster::test_cluster::join(
	const std::string &node,
	const std::string &node_zone,
	const std::vector<member> &nodes)
{
	std::unique_lock<std::shared_mutex> lock(membership_mutex);

	self = node;
	zone = node_zone;
	member_list = nodes;
}

void cluster::test_cluster::led_by(const std::string &node, int64_t node_term)
{
	std::unique_lock<std::shared_mutex> lock(membership_mutex);

	leader_node = node;
	term = node_term;
}

bool cluster::test_cluster::joined() const
{
	return started;
}

void cluster::test_cluster::start()
{
	started = true;
}

bool cluster::test_cluster::discover()
{
	return false;
}

void cluster::test_cluster::stop()
{
	started = false;
}

std::vector<cluster::member> cluster::test_cluster::members() const
{
	std::shared_lock<std::shared_mutex> lock(membership_mutex);

	return member_list;
}

cluster::placement cluster::test_cluster::replicas(const std::string &key) const
{
	return copies_of(partition_of(key));
}

cluster::placement cluster::test_cluster::copies_of(size_t partition) const
{
	std::shared_lock<std::shared_mutex> lock(membership_mutex);
	std::vector<member> owners = owners_of(partition_name(partition), member_list);
	placement where;

	where.local = false;

	for (const auto &owner : owners)
	{
		if (owner.node == self)
		{
			where.local = true;
		}
		else
		{
			where.nodes.push_back(owner.node);
		}
	}

	return where;
}

cluster::partition_set cluster::test_cluster::holdings() const
{
	std::shared_lock<std::shared_mutex> lock(membership_mutex);
	partition_set held;

	for (size_t partition = 0; partition < partition_count; partition++)
	{
		std::vector<member> owners = owners_of(partition_name(partition), member_list);

		for (const auto &owner : owners)
		{
			if (owner.node == self)
			{
				held.set(partition);
			}
		}
	}

	return held;
}

uint64_t cluster::test_cluster::generation() const
{
	return 0;
}

void cluster::test_cluster::vouch(const partition_set &partitions, uint64_t)
{
	std::lock_guard<std::mutex> lock(vouch_mutex);

	filled |= partitions;
}

cluster::partition_set cluster::test_cluster::vouched() const
{
	std::lock_guard<std::mutex> lock(vouch_mutex);

	return filled & holdings();
}

std::map<std::string, cluster::partition_set> cluster::test_cluster::holders(const partition_set &partitions) const
{
	std::shared_lock<std::shared_mutex> lock(membership_mutex);

	return holders_of(partitions, member_list, self, zone);
}

std::map<std::string, cluster::partition_set> cluster::test_cluster::holders_in(
	const partition_set &partitions,
	const std::vector<std::string> &zone_nodes) const
{
	return ::cluster::holders_in(partitions, zone_nodes);
}

std::vector<std::string> cluster::test_cluster::peers() const
{
	std::shared_lock<std::shared_mutex> lock(membership_mutex);
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

std::vector<std::vector<std::string>> cluster::test_cluster::zones() const
{
	std::shared_lock<std::shared_mutex> lock(membership_mutex);

	return zones_of(member_list, self, zone);
}

cluster::leadership cluster::test_cluster::leader(const std::string &key)
{
	std::shared_lock<std::shared_mutex> lock(membership_mutex);

	leadership led;

	led.known = true;
	led.node = leading(partition_of(key));
	led.local = led.node == self;
	led.term = term;

	return led;
}

size_t cluster::test_cluster::leads() const
{
	std::shared_lock<std::shared_mutex> lock(membership_mutex);
	size_t led = 0;

	for (size_t partition = 0; partition < partition_count; partition++)
	{
		led += leading(partition) == self ? 1 : 0;
	}

	return led;
}

std::string cluster::test_cluster::leading(size_t partition) const
{
	return leader_node.empty() ? leader_of(partition_name(partition), member_list) : leader_node;
}

bool cluster::test_cluster::is_unled() const
{
	return false;
}

bool cluster::test_cluster::is_alone() const
{
	return false;
}

cluster::etcd_registration cluster::test_cluster::registration() const
{
	return etcd_registration();
}

bool cluster::test_cluster::accept(const std::string &key, int64_t sent)
{
	std::shared_lock<std::shared_mutex> lock(membership_mutex);

	return sent == 0 || (sent >= term && sent >= restored[partition_of(key)]);
}

void cluster::test_cluster::restore_terms(const partition_terms &applied)
{
	std::unique_lock<std::shared_mutex> lock(membership_mutex);

	restored = applied;
}
