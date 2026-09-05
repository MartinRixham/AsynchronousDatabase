#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>

#include <pthread.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/json.hpp>

#include "log.h"
#include "forwarder.h"
#include "partition.h"
#include "etcd_cluster.h"

namespace
{
	std::string environment(const char *name)
	{
		const char *value = getenv(name);

		return value == NULL ? "" : value;
	}

	// One address or several separated by commas. An empty one is not an address, so a trailing
	// comma names nothing rather than naming the empty string.
	std::vector<std::string> read_endpoints(const std::string &configured)
	{
		std::vector<std::string> split;
		std::vector<std::string> endpoints;

		boost::algorithm::split(split, configured, boost::algorithm::is_any_of(","));

		for (size_t i = 0; i < split.size(); i++)
		{
			if (!split[i].empty())
			{
				endpoints.push_back(split[i]);
			}
		}

		return endpoints;
	}

	// What a node writes about itself: where it answers and which zone it stands in. A value that
	// is not a document at all is a node of a version that knew nothing about zones, and it is
	// read as the address it is rather than dropped from the membership.
	cluster::member read_member(const std::string &value)
	{
		boost::system::error_code error;
		boost::json::value json = boost::json::parse(value, error);

		if (error || !json.is_object() || !json.as_object().contains("node") || !json.at("node").is_string())
		{
			return cluster::member { value, "" };
		}

		const boost::json::object &object = json.as_object();
		std::string zone;

		if (object.contains("zone") && object.at("zone").is_string())
		{
			zone = std::string(object.at("zone").as_string());
		}

		return cluster::member { std::string(object.at("node").as_string()), zone };
	}
}

bool cluster::config::is_clustered() const
{
	return !endpoints.empty() && !node.empty();
}

cluster::config cluster::from_environment()
{
	config config;

	config.endpoints = read_endpoints(environment("ASYNCDB_ETCD"));
	config.node = environment("ASYNCDB_NODE");
	config.zone = environment("ASYNCDB_ZONE");

	return config;
}

cluster::etcd_cluster::etcd_cluster(const config &cluster_config):
	configuration(cluster_config),
	node_curl(http::curl_client(cluster_config.timeout_seconds)),
	etcd_curl(http::curl_client(cluster_config.etcd_timeout_seconds)),
	http_client(node_curl),
	etcd_client(etcd::client(etcd_curl, cluster_config.endpoints))
{
}

cluster::etcd_cluster::etcd_cluster(const config &cluster_config, const http::client &http):
	configuration(cluster_config),
	node_curl(http::curl_client(cluster_config.timeout_seconds)),
	etcd_curl(http::curl_client(cluster_config.etcd_timeout_seconds)),
	http_client(http),
	etcd_client(etcd::client(http, cluster_config.endpoints))
{
}

cluster::etcd_cluster::~etcd_cluster()
{
	stop();
}

void cluster::etcd_cluster::start()
{
	if (!configuration.is_clustered())
	{
		return;
	}

	{
		std::lock_guard<std::mutex> lock(wait_mutex);

		if (running)
		{
			return;
		}

		running = true;
	}

	// Registering before the thread starts means the first request is answered by a node that
	// already knows who its neighbours are, rather than by one that thinks it is alone.
	refresh();

	// A signal is delivered to whichever thread is able to take it, and this one holds the lock
	// that stopping it needs, so it is started with every signal blocked and takes none of them.
	sigset_t blocked;
	sigset_t previous;

	sigfillset(&blocked);
	pthread_sigmask(SIG_BLOCK, &blocked, &previous);

	thread = std::thread([this]() { run(); });

	pthread_sigmask(SIG_SETMASK, &previous, NULL);

	DEBUG("Node " + configuration.node + " joined the cluster at " + etcd_client.endpoint() + ".");
}

void cluster::etcd_cluster::stop()
{
	{
		std::lock_guard<std::mutex> lock(wait_mutex);

		if (!running)
		{
			return;
		}

		running = false;
	}

	wake.notify_all();

	if (thread.joinable())
	{
		thread.join();
	}

	// Revoking takes the node's key with it, so a node that is shut down leaves at once rather
	// than when its lease runs out. Failing to revoke is not a failure to leave: everything is
	// often shut down together, and etcd may already be gone.
	if (lease != 0 && !etcd_client.revoke(lease))
	{
		DEBUG(
			"Node " + configuration.node + " could not tell etcd it was leaving, and is dropped in " +
			std::to_string(configuration.lease_seconds) + " seconds when its lease runs out.");
	}

	lease = 0;

	DEBUG("Node " + configuration.node + " left the cluster.");
}

std::vector<cluster::member> cluster::etcd_cluster::members() const
{
	std::shared_lock<std::shared_mutex> lock(member_mutex);

	return member_list;
}

cluster::placement cluster::etcd_cluster::replicas(const std::string &key) const
{
	std::vector<member> registered = members();

	// One node, or none that etcd would name, holds everything it is asked for. A cluster that
	// cannot be read is a cluster of one rather than a cluster that refuses to answer.
	if (registered.size() < 2)
	{
		return placement();
	}

	// The copies are the copies of the key's *partition*, so that every key in it is held by the
	// same three nodes — which is what makes a partition a thing that can be led.
	//
	// The name of the namespace is hidden here by the name of the base class.
	std::vector<member> owners =
		::cluster::owners_of(::cluster::partition_name(::cluster::partition_of(key)), registered);
	placement where;

	where.local = false;

	for (size_t i = 0; i < owners.size(); i++)
	{
		if (owners[i].node == configuration.node)
		{
			where.local = true;
		}
		// The copy in this node's own zone goes first, so that a request this node cannot answer
		// itself crosses a zone only when it has to.
		else if (owners[i].zone == configuration.zone)
		{
			where.nodes.insert(where.nodes.begin(), owners[i].node);
		}
		else
		{
			where.nodes.push_back(owners[i].node);
		}
	}

	return where;
}

std::vector<std::string> cluster::etcd_cluster::peers() const
{
	std::vector<member> registered = members();
	std::vector<std::string> peers;

	for (size_t i = 0; i < registered.size(); i++)
	{
		if (registered[i].node != configuration.node)
		{
			peers.push_back(registered[i].node);
		}
	}

	return peers;
}

std::optional<cluster::leadership> cluster::etcd_cluster::leader(const std::string &key) const
{
	// One node, or none that etcd would name, races with nobody, so there is nothing to order.
	if (members().size() < 2)
	{
		return std::nullopt;
	}

	size_t partition = ::cluster::partition_of(key);

	std::shared_lock<std::shared_mutex> lock(leader_mutex);

	std::map<size_t, leadership>::const_iterator found = leader_list.find(partition);

	// A partition nothing is known to lead is a leadership that is not known, and not the absence
	// of leadership: a write of it waits for an election rather than going around one.
	return found == leader_list.end() ? leadership() : found->second;
}

size_t cluster::etcd_cluster::leads() const
{
	std::shared_lock<std::shared_mutex> lock(leader_mutex);

	return std::count_if(
		leader_list.begin(),
		leader_list.end(),
		[](const std::pair<size_t, leadership> &led) { return led.second.local; });
}

bool cluster::etcd_cluster::accept(const std::string &key, int64_t term)
{
	size_t partition = ::cluster::partition_of(key);

	std::lock_guard<std::mutex> lock(term_mutex);

	std::map<size_t, int64_t>::const_iterator seen = terms.find(partition);

	// A write no leader ordered is a cluster that has no leadership, and it is applied as it always
	// was. A cluster that does have one never sends a term of nothing.
	if (term == 0)
	{
		return true;
	}

	if (seen != terms.end() && term < seen->second)
	{
		return false;
	}

	terms[partition] = term;

	return true;
}

std::vector<std::vector<std::string>> cluster::etcd_cluster::zones() const
{
	std::vector<member> registered = members();

	// One node, or none that etcd would name, answers a scan out of its own store, the same way it
	// answers for every key.
	if (registered.size() < 2)
	{
		return std::vector<std::vector<std::string>>();
	}

	// The name of the namespace is hidden here by the name of the base class.
	return ::cluster::zones_of(registered, configuration.node, configuration.zone);
}

router::response cluster::etcd_cluster::send(const std::string &node, const router::request &request) const
{
	return forward(http_client, node, request);
}

std::optional<router::response> cluster::etcd_cluster::send_all(
	const std::vector<std::string> &node_list,
	const router::request &request) const
{
	return refusal(forward_all(http_client, node_list, request));
}

void cluster::etcd_cluster::run()
{
	std::unique_lock<std::mutex> lock(wait_mutex);

	// A third of the lease is two chances to be renewed before it runs out, which is what keeps a
	// node in the membership across a slow answer from etcd.
	std::chrono::seconds interval(std::max<int64_t>(configuration.lease_seconds / 3, 1));

	// Waiting answers true when it was woken to stop and false when the interval ran out, which
	// is the tick that renews the lease and reads the membership again.
	while (!wake.wait_for(lock, interval, [this]() { return !running; }))
	{
		lock.unlock();
		refresh();
		lock.lock();
	}
}

void cluster::etcd_cluster::refresh()
{
	// A lease that cannot be renewed is a node that was away long enough to be dropped from the
	// membership, so it registers again rather than believing it is still a member.
	if ((lease == 0 || !etcd_client.keep_alive(lease)) && !register_node())
	{
		DEBUG("Node " + configuration.node + " could not register with etcd.");
	}

	read_members();
	read_leaders();
}

bool cluster::etcd_cluster::register_node()
{
	std::optional<int64_t> granted = etcd_client.grant_lease(configuration.lease_seconds);

	if (!granted)
	{
		lease = 0;

		return false;
	}

	lease = *granted;

	// A node registers its zone with its address, because it is the only one that knows which zone
	// it is in and every other node has to know to keep a copy out of it.
	boost::json::object registration {
		{ "node", configuration.node },
		{ "zone", configuration.zone }
	};

	return etcd_client.put(
		configuration.prefix + configuration.node, boost::json::serialize(registration), lease);
}

// Leadership is claimed rather than agreed: the first node to create the partition's key in etcd
// leads it, and the transaction that creates it is what makes two claimants one leader. A node
// claims only the partitions it holds a copy of, because a leader writes to itself first.
void cluster::etcd_cluster::read_leaders()
{
	std::vector<member> registered = members();

	if (registered.size() < 2)
	{
		return;
	}

	// A claim is written on this node's own lease, so a node that stops renewing stops leading. A
	// node with no lease has nothing to claim on and would leave a leadership behind that nothing
	// ever expires.
	if (lease == 0)
	{
		return;
	}

	std::map<std::string, std::string> held = etcd_client.range(configuration.leader_prefix);
	std::map<size_t, leadership> known;
	size_t claims = 0;

	// The partitions are walked from an offset of this node's own rather than from the first one,
	// so that nodes claim different parts of the ring instead of racing each other for the same
	// partitions on every pass — which spreads leadership by arithmetic rather than by luck, and
	// spends fewer round trips losing.
	size_t offset = ::cluster::score(configuration.node, "leader") % partition_count;

	for (size_t i = 0; i < partition_count; i++)
	{
		size_t partition = (offset + i) % partition_count;
		std::string key = configuration.leader_prefix + std::to_string(partition);
		std::map<std::string, std::string>::const_iterator holder = held.find(key);

		if (holder != held.end())
		{
			// The term is not in the value, so a leadership read back from the range is the node
			// alone until this instance claims it or is told the revision another way. Reading it
			// is what routes a write; leading it is what fences one.
			leadership led;

			led.known = true;
			led.local = holder->second == configuration.node;
			led.node = holder->second;
			led.term = term_of(holder->second, partition);

			known[partition] = led;

			continue;
		}

		// Nothing leads it, so this node claims it if it is one of the copies — and if it has not
		// already claimed as many as it takes on one pass. What is left over is claimed on the
		// next one, by this node or by another copy of it, and until then a write of it is
		// answered as having no leader.
		if (claims >= configuration.claims_per_refresh || !holds(registered, partition))
		{
			continue;
		}

		claims++;

		std::optional<etcd::claim> claimed = etcd_client.create(key, configuration.node, lease);

		if (!claimed)
		{
			continue;
		}

		leadership led;

		led.known = true;
		led.local = claimed->holder == configuration.node;
		led.node = claimed->holder;
		led.term = claimed->revision;

		known[partition] = led;

		if (claimed->held)
		{
			remember_term(partition, claimed->revision);

			DEBUG(
				"Node " + configuration.node + " leads partition " + std::to_string(partition) +
				" in term " + std::to_string(claimed->revision) + ".");
		}
	}

	std::unique_lock<std::shared_mutex> lock(leader_mutex);

	leader_list = known;
}

bool cluster::etcd_cluster::holds(const std::vector<member> &registered, size_t partition) const
{
	std::vector<member> owners = ::cluster::owners_of(::cluster::partition_name(partition), registered);

	return std::any_of(
		owners.begin(),
		owners.end(),
		[this](const member &owner) { return owner.node == configuration.node; });
}

int64_t cluster::etcd_cluster::term_of(const std::string &holder, size_t partition) const
{
	if (holder != configuration.node)
	{
		return 0;
	}

	std::lock_guard<std::mutex> lock(term_mutex);

	std::map<size_t, int64_t>::const_iterator remembered = terms.find(partition);

	return remembered == terms.end() ? 0 : remembered->second;
}

void cluster::etcd_cluster::remember_term(size_t partition, int64_t term)
{
	std::lock_guard<std::mutex> lock(term_mutex);

	if (term > terms[partition])
	{
		terms[partition] = term;
	}
}

void cluster::etcd_cluster::read_members()
{
	std::map<std::string, std::string> registered = etcd_client.range(configuration.prefix);
	std::vector<member> names;
	bool found = false;

	for (std::map<std::string, std::string>::const_iterator it = registered.begin(); it != registered.end(); ++it)
	{
		member read = read_member(it->second);

		found = found || read.node == configuration.node;

		names.push_back(read);
	}

	// This node is a member of its own cluster whatever etcd says, so that a node which cannot
	// reach etcd still answers for the keys it holds instead of forwarding them to a stranger.
	if (!found)
	{
		names.push_back(member { configuration.node, configuration.zone });
	}

	std::sort(
		names.begin(),
		names.end(),
		[](const member &left, const member &right) { return left.node < right.node; });

	std::unique_lock<std::shared_mutex> lock(member_mutex);

	member_list = names;
}
