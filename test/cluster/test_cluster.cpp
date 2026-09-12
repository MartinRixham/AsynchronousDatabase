#include "cluster/partition.h"
#include "test_cluster.h"

cluster::test_cluster::test_cluster():
	curl(http::curl_client(2)),
	request_forwarder(forwarder(curl))
{
}

void cluster::test_cluster::join(
	const std::string &node,
	const std::string &node_zone,
	const std::vector<member> &nodes)
{
	self = node;
	zone = node_zone;
	member_list = nodes;
}

void cluster::test_cluster::led_by(const std::string &node, int64_t node_term)
{
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
	return member_list;
}

cluster::placement cluster::test_cluster::replicas(const std::string &key) const
{
	std::vector<member> owners = owners_of(partition_name(partition_of(key)), member_list);
	placement where;

	where.local = false;

	for (size_t i = 0; i < owners.size(); i++)
	{
		if (owners[i].node == self)
		{
			where.local = true;
		}
		else
		{
			where.nodes.push_back(owners[i].node);
		}
	}

	return where;
}

cluster::partition_set cluster::test_cluster::holdings() const
{
	partition_set held;

	for (size_t partition = 0; partition < partition_count; partition++)
	{
		std::vector<member> owners = owners_of(partition_name(partition), member_list);

		for (size_t i = 0; i < owners.size(); i++)
		{
			if (owners[i].node == self)
			{
				held.set(partition);
			}
		}
	}

	return held;
}

std::map<std::string, cluster::partition_set> cluster::test_cluster::holders(
	const partition_set &partitions) const
{
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

std::vector<std::vector<std::string>> cluster::test_cluster::zones() const
{
	return zones_of(member_list, self, zone);
}

std::optional<cluster::leadership> cluster::test_cluster::leader(const std::string &) const
{
	if (leader_node.empty())
	{
		return std::nullopt;
	}

	leadership led;

	led.known = true;
	led.local = leader_node == self;
	led.node = leader_node;
	led.term = term;

	return led;
}

size_t cluster::test_cluster::leads() const
{
	return leader_node == self ? partition_count : 0;
}

bool cluster::test_cluster::is_unled() const
{
	return false;
}

cluster::etcd_registration cluster::test_cluster::registration() const
{
	return etcd_registration();
}

bool cluster::test_cluster::accept(const std::string &, int64_t sent)
{
	return sent == 0 || sent >= term;
}

router::response cluster::test_cluster::send(const std::string &node, const router::request &request) const
{
	return request_forwarder.forward(node, request);
}

std::optional<router::response> cluster::test_cluster::send_all(
	const std::vector<std::string> &node_list,
	const router::request &request) const
{
	return refusal(request_forwarder.forward_all(node_list, request));
}

std::vector<router::response> cluster::test_cluster::send_each(const std::vector<enquiry> &enquiries) const
{
	return request_forwarder.forward_each(enquiries);
}
