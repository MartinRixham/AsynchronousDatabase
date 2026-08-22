#ifndef CLUSTER_ETCD_CLUSTER_H
#define CLUSTER_ETCD_CLUSTER_H

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "etcd/etcd_client.h"
#include "http/http_client.h"
#include "cluster.h"

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

		// How long the membership of a node outlives the node itself.
		int64_t lease_seconds = 10;

		// etcd is on a shorter leash than a node is, because a node that cannot reach it carries
		// on serving the keys it holds, and waiting is what it would be doing instead.
		long etcd_timeout_seconds = 5;

		std::string prefix = "/asyncdb/node/";

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

		std::vector<std::string> member_list;

		int64_t lease = 0;

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

		std::vector<std::string> members() const override;

		std::optional<std::string> owner(const std::string &key) const override;

		std::vector<std::string> peers() const override;

		router::response send(const std::string &node, const router::request &request) const override;

	private:
		void run();

		void refresh();

		bool register_node();

		void read_members();
	};
}

#endif
