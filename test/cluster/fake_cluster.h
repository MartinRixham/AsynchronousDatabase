#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cluster/cluster.h"

namespace cluster
{
	// A cluster of nodes that are not there: a key is held by whichever nodes the test says, and
	// what one of them answers when it is asked is cluster::fake_forwarder's to say.
	class fake_cluster : public cluster
	{
		std::string self;

		std::vector<member> member_list;

		std::map<std::string, std::vector<std::string>> owners;

		std::map<std::string, leadership> leaders;

		std::map<std::string, int64_t> refused;

		partition_terms restored = {};

		bool unled_node = false;

		bool alone_node = false;

		etcd_registration etcd_state;

		partition_set filled;

		// A server reads the membership from a thread of its own while the test moves it. Held by
		// pointer because a fixture hands one of these back by value.
		std::shared_ptr<std::mutex> mutex;

		// The nodes a test named, as a placement: this node taken out of the list and marked
		// local, which is what the seam answers with.
		placement placed(const std::vector<std::string> &nodes) const;

	public:
		fake_cluster(const std::string &node, const std::vector<std::string> &members);

		fake_cluster(const std::string &node, const std::vector<member> &members);

		// The one node holding the key, which is a cluster keeping one copy.
		void owns(const std::string &key, const std::string &node);

		// Every node holding a copy of the key, in the order this node would ask them.
		void copies(const std::string &key, const std::vector<std::string> &nodes);

		// The node ordering writes to this key, and the term it orders them in.
		void led_by(const std::string &key, const std::string &node, int64_t term);

		// A key whose partition is led by nobody yet, which is a write with nowhere to go.
		void led_by_nobody(const std::string &key);

		// A term this node has already applied a write of, so that anything older is refused.
		void applied(const std::string &key, int64_t term);

		// A node that has been unable to order a write for long enough to be taken out of the
		// load balancer.
		void unled();

		// A node with no membership but itself, which takes itself to hold every key.
		void alone();

		// A store that does not hold the whole of what this node owns, which is every partition
		// until a pass fills it again.
		void unvouched();

		// The same of the one partition the key is in.
		void unvouched(const std::string &key);

		// The etcd this node reads the membership from, and that it is registered there. A test
		// that says nothing is an instance that was told no etcd at all.
		void reads_etcd(const std::string &endpoint);

		// The same etcd, no longer answering: the node is still looking there and no longer holds
		// the lease its registration is written on.
		void lost_etcd();

		// The same etcd answering again, and the registration written again on a new lease.
		void registered_again();

		// The membership is the one the test named, so there is nothing to join, nothing to read
		// and nothing to leave.
		void start() override;

		bool discover() override;

		void stop() override;

		std::vector<member> members() const override;

		placement replicas(const std::string &key) const override;

		placement copies_of(size_t partition) const override;

		partition_set holdings() const override;

		// A membership a test names never moves, so it is always the first generation.
		uint64_t generation() const override;

		void vouch(const partition_set &partitions, uint64_t since) override;

		partition_set vouched() const override;

		std::map<std::string, partition_set> holders(const partition_set &partitions) const override;

		std::map<std::string, partition_set> holders_in(
			const partition_set &partitions,
			const std::vector<std::string> &zone) const override;

		std::vector<std::string> peers() const override;

		std::vector<std::vector<std::string>> zones() const override;

		leadership leader(const std::string &key) override;

		size_t leads() const override;

		bool is_unled() const override;

		bool is_alone() const override;

		etcd_registration registration() const override;

		bool accept(const std::string &key, int64_t term) override;

		void restore_terms(const partition_terms &terms) override;
	};
}
