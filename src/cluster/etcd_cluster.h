#ifndef CLUSTER_ETCD_CLUSTER_H
#define CLUSTER_ETCD_CLUSTER_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "etcd/etcd_client.h"
#include "http/http_client.h"
#include "cluster.h"
#include "member.h"

namespace cluster
{
	struct config
	{
		// Where etcd answers, as base URLs — every member of the etcd cluster, because any of them
		// answers for all of it. Empty is one instance on its own.
		std::vector<std::string> endpoints;

		// This node as the other nodes reach it, which is what is written into etcd and what the
		// hash of a key names.
		std::string node;

		// The availability zone this node stands in. Every zone holds a copy of every record, so
		// naming a second zone is what turns partitioning into replication; naming none leaves the
		// cluster with the one copy it had.
		std::string zone;

		// How long the membership of a node outlives the node itself.
		int64_t lease_seconds = 10;

		// etcd is on a shorter leash than a node is, because a node that cannot reach it carries
		// on serving the keys it holds, and waiting is what it would be doing instead.
		long etcd_timeout_seconds = 5;

		std::string prefix = "/asyncdb/node/";

		std::string leader_prefix = "/asyncdb/leader/";

		// How many partitions a node claims on one pass. Claiming costs a round trip to etcd
		// each, and there are 256 of them, so a node takes a few at a time rather than all of
		// them at once — several nodes claiming from their own offsets settle a cold cluster in
		// a pass or two between them.
		size_t claims_per_refresh = 64;

		long timeout_seconds = 30;

		bool is_clustered() const;
	};

	// ASYNCDB_ETCD, which is one address or several separated by commas, and ASYNCDB_NODE.
	// Nothing configured is nothing clustered.
	config from_environment();

	class etcd_cluster : public cluster
	{
		config configuration;

		// The clients a cluster talks to its neighbours and to etcd with, unless it was given one,
		// which is how a cluster is driven in a test without a network.
		http::curl_client node_curl;

		http::curl_client etcd_curl;

		const http::client &http_client;

		etcd::client etcd_client;

		// Read by every request and written only by the membership thread.
		mutable std::shared_mutex member_mutex;

		std::vector<member> member_list;

		int64_t lease = 0;

		// Who leads each partition, as this node last read it from etcd. Read by every write and
		// written only by the membership thread.
		mutable std::shared_mutex leader_mutex;

		std::map<size_t, leadership> leader_list;

		// The highest term this node has applied a write of each partition in. A write ordered in
		// an older term is a leader that has been replaced and does not know it.
		mutable std::mutex term_mutex;

		std::map<size_t, int64_t> terms;

		std::thread thread;

		mutable std::mutex wait_mutex;

		std::condition_variable wake;

		bool running = false;

	public:
		explicit etcd_cluster(const config &cluster_config);

		etcd_cluster(const config &cluster_config, const http::client &http);

		~etcd_cluster();

		etcd_cluster(const etcd_cluster &) = delete;

		etcd_cluster &operator=(const etcd_cluster &) = delete;

		// Registers this node and keeps its membership alive until stop(). Standing alone, both
		// are nothing at all.
		void start();

		void stop();

		std::vector<member> members() const override;

		placement replicas(const std::string &key) const override;

		std::vector<std::string> peers() const override;

		std::vector<std::vector<std::string>> zones() const override;

		std::optional<leadership> leader(const std::string &key) const override;

		size_t leads() const override;

		bool accept(const std::string &key, int64_t term) override;

		router::response send(const std::string &node, const router::request &request) const override;

		std::optional<router::response> send_all(
			const std::vector<std::string> &node_list,
			const router::request &request) const override;

	private:
		void run();

		void refresh();

		bool register_node();

		void read_members();

		// Claims every partition this node holds a copy of and nothing leads yet, and reads back
		// who leads the rest. A node leads on the same lease its membership is on, so a node that
		// stops renewing stops leading.
		void read_leaders();

		// Whether this node holds a copy of the partition, which is what it claims leadership of.
		bool holds(const std::vector<member> &registered, size_t partition) const;

		// The term this node leads a partition in. Only the node that claimed a partition knows
		// it — the term is the revision of that claim — so another node's is nothing here.
		int64_t term_of(const std::string &holder, size_t partition) const;

		void remember_term(size_t partition, int64_t term);
	};
}

#endif
