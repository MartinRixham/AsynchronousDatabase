#ifndef CLUSTER_ETCD_CLUSTER_H
#define CLUSTER_ETCD_CLUSTER_H

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "etcd/etcd_client.h"
#include "http/curl_client.h"
#include "http/http_client.h"
#include "cluster.h"
#include "forwarder.h"
#include "member.h"
#include "partition.h"

namespace cluster
{
	struct config
	{
		std::vector<std::string> endpoints;

		std::string node;

		std::string zone;

		// Whether a node leading nothing takes a write anyway. A node standing alone claims no
		// partition, because it races with nobody, so true is the lone instance that owns the
		// whole keyspace serving writes as it always has; false is a deployment where a write is
		// only ever ordered by a leader claimed in etcd, and a node that reaches no etcd — or
		// stands in a membership too small to claim anything — refuses the write rather than
		// taking one nothing ordered.
		bool unled_writes = true;

		// How long the membership of a node outlives the node itself.
		int64_t lease_seconds = 10;

		// etcd is on a shorter leash than a node is, because a node that cannot reach it carries
		// on serving the keys it holds, and waiting is what it would be doing instead.
		long etcd_timeout_seconds = 5;

		std::string prefix = "/asyncdb/node/";

		std::string leader_prefix = "/asyncdb/leader/";

		size_t claims_per_refresh = 64;

		long timeout_seconds = 30;

		// How long a node is given to answer at all, apart from how long it is given to finish
		// answering. A neighbour that is gone costs the thread serving the request this and not
		// the whole timeout, which is as long as it is because a value may be sixteen megabytes.
		long connect_timeout_seconds = 2;

		bool is_clustered() const;
	};

	config from_environment();

	class etcd_cluster : public cluster
	{
		config configuration;

		const forwarder &request_forwarder;

		etcd::client etcd_client;

		// The membership as it was last read, held whole and swapped rather than edited: the
		// vector a reader is given never changes, so reading it is an atomic load and a reference
		// count rather than a lock and a copy of every name. Written only by the membership
		// thread. Never null once the constructor has run.
		std::atomic<membership> member_list;

		// Atomic because health reads it: whether this node holds a lease is what says etcd
		// answered, and the membership thread is the only one that takes or renews one.
		std::atomic<int64_t> lease = 0;

		// Who leads each partition, as this node last read it from etcd. Read by every write and
		// written only by the membership thread.
		mutable std::shared_mutex leader_mutex;

		std::map<size_t, leadership> leader_list;

		// When this node stopped standing in a membership large enough to claim a leader, and the
		// epoch while it stands in one. A membership that has just fallen to one is often one
		// about to come back — a slow answer from etcd, a peer restarting — so what is reported
		// outside is the state that outlasted a lease rather than the state of the last pass.
		std::atomic<std::chrono::steady_clock::time_point> unled_since;

		// The partitions this node holds a claim on and should no longer lead, as the pass before
		// this one found them. A claim is given up on the second pass that finds it rather than the
		// first, so a membership read a moment out of date is not a partition left with no leader.
		// Touched only by the membership thread.
		std::set<size_t> releasing;

		// One slot a partition rather than a map behind a lock: the count is fixed, so every write
		// raises the term of its own partition and no write waits on a write of another.
		std::array<std::atomic<int64_t>, partition_count> terms = {};

		std::thread thread;

		mutable std::mutex wait_mutex;

		std::condition_variable wake;

		bool running = false;

	public:
		etcd_cluster(const config &cluster_config, const http::client &http, const forwarder &forwarding);

		~etcd_cluster();

		etcd_cluster(const etcd_cluster &) = delete;

		etcd_cluster &operator=(const etcd_cluster &) = delete;

		void start() override;

		bool discover() override;

		void stop() override;

		std::vector<member> members() const override;

		placement replicas(const std::string &key) const override;

		partition_set holdings() const override;

		std::map<std::string, partition_set> holders(const partition_set &partitions) const override;

		std::map<std::string, partition_set> holders_in(
			const partition_set &partitions,
			const std::vector<std::string> &zone) const override;

		std::vector<std::string> peers() const override;

		std::vector<std::vector<std::string>> zones() const override;

		std::optional<leadership> leader(const std::string &key) const override;

		size_t leads() const override;

		bool is_unled() const override;

		etcd_registration registration() const override;

		bool accept(const std::string &key, int64_t term) override;

		router::response send(const std::string &node, const router::request &request) const override;

		std::optional<router::response> send_all(
			const std::vector<std::string> &node_list,
			const router::request &request) const override;

		std::vector<router::response> send_each(const std::vector<enquiry> &enquiries) const override;

	private:
		// What stop() does, and what the destructor calls: a destructor cannot reach an override.
		void leave();

		void run();

		void refresh();

		bool register_node();

		void read_members();

		void read_leaders();

		// The membership to answer one request from. Every question about where a key lives is
		// asked of one of these rather than of the field, so a request that asks several of them
		// cannot see the membership change half way through.
		membership snapshot() const;

		// Whether this node is the one the membership names to lead a partition. Every node works
		// out the same answer, so a claim is the node saying it is that one rather than the node
		// that got there first: leadership is spread as evenly as the keyspace, and a node that
		// joins takes its share of it from the nodes that give theirs up.
		bool should_lead(const std::vector<member> &registered, size_t partition) const;

		// Compare and exchange rather than a store, because two writes of one partition arriving at
		// once have to leave the higher term behind whichever of them wins.
		bool raise_term(size_t partition, int64_t term);

		// The term this node leads a partition in. Only the node that claimed a partition knows
		// it — the term is the revision of that claim — so another node's is nothing here.
		int64_t term_of(const std::string &holder, size_t partition) const;
	};
}

#endif
