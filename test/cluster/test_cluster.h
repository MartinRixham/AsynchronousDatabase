#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "cluster/cluster.h"

namespace cluster
{
	// Two real servers on two real ports, which are known only once they are listening, so the
	// membership is told to the cluster rather than read from etcd. Everything else — who owns a
	// key and which node leads it — is worked out the way production works it out, and reaching
	// the node that holds it is the real forwarder the test hands the server beside this.
	class test_cluster final : public cluster
	{
		// A test redraws the membership and names the leader while the servers are running, and
		// their sessions and reconcile passes read both throughout.
		mutable std::shared_mutex membership_mutex;

		std::string self;

		std::string zone;

		// Empty is the node the membership names, as it names one in production, in the first term.
		std::string leader_node;

		int64_t term = 1;

		// Restored by the router before the server listens, and read by every session after.
		partition_terms restored = {};

		std::vector<member> member_list;

		std::atomic<bool> started = false;

		// Vouched for by the server's thread before it listens, and read by every session after.
		mutable std::mutex vouch_mutex;

		partition_set filled;

		std::string leading(size_t partition) const;

	public:
		test_cluster();

		void join(const std::string &node, const std::string &node_zone, const std::vector<member> &nodes);

		// The leader is told to the cluster rather than claimed in etcd, the same way the
		// membership is: what is under test here is what a leader does, not how it is chosen.
		void led_by(const std::string &node, int64_t node_term);

		// Whether the server was given this cluster to join, which is the one it routes through.
		bool joined() const;

		// The membership is the one join() was told, so there is nothing to read, and joining and
		// leaving are recorded and do nothing else.
		void start() override;

		bool discover() override;

		void stop() override;

		std::vector<member> members() const override;

		placement replicas(const std::string &key) const override;

		placement copies_of(size_t partition) const override;

		partition_set holdings() const override;

		uint64_t generation() const override;

		void vouch(const partition_set &partitions, uint64_t since) override;

		partition_set vouched() const override;

		std::map<std::string, partition_set> holders(const partition_set &partitions) const override;

		std::map<std::string, partition_set> holders_in(
			const partition_set &partitions,
			const std::vector<std::string> &zone_nodes) const override;

		std::vector<std::string> peers() const override;

		std::vector<std::vector<std::string>> zones() const override;

		leadership leader(const std::string &key) override;

		size_t leads() const override;

		// A membership a test named is one every node is in, so there is no moment of being
		// unable to order a write.
		bool is_unled() const override;

		bool is_alone() const override;

		// A membership a test handed in is one no etcd was read for.
		etcd_registration registration() const override;

		bool accept(const std::string &key, int64_t term) override;

		void restore_terms(const partition_terms &applied) override;
	};
}
