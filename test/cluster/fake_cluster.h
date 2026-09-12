#ifndef CLUSTER_FAKE_CLUSTER_H
#define CLUSTER_FAKE_CLUSTER_H

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cluster/cluster.h"

namespace cluster
{
	// A cluster of nodes that are not there: a key is held by whichever nodes the test says, and a
	// node answers what the test told it to answer.
	class fake_cluster : public cluster
	{
		std::string self;

		std::vector<member> member_list;

		std::map<std::string, std::vector<std::string>> owners;

		std::map<std::string, router::response> answers;

		// The answers a node gives one after another, and how many of them it has given.
		std::map<std::string, std::vector<router::response>> answer_list;

		mutable std::map<std::string, size_t> answered;

		// How long a node takes to answer, which is what a pass runs out of time inside.
		std::map<std::string, std::chrono::milliseconds> delays;

		std::map<std::string, leadership> leaders;

		std::map<std::string, int64_t> refused;

		bool unled_node = false;

		etcd_registration etcd_state;

		mutable std::vector<std::pair<std::string, router::request>> requests;

		// A walk asks several nodes at once, so what it was asked and what it has answered are
		// written from several threads. Held by pointer because a fixture hands one of these back
		// by value.
		std::shared_ptr<std::mutex> mutex;

	public:
		fake_cluster(const std::string &node, const std::vector<std::string> &members);

		fake_cluster(const std::string &node, const std::vector<member> &members);

		// The one node holding the key, which is a cluster keeping one copy.
		void owns(const std::string &key, const std::string &node);

		// Every node holding a copy of the key, in the order this node would ask them.
		void copies(const std::string &key, const std::vector<std::string> &nodes);

		void answer(const std::string &node, const router::response &response);

		// The answers a node gives in turn rather than one answer to everything, so that a caller
		// paging through a scan is answered a page at a time. The last of them answers everything
		// after it, which is a range that stays exhausted.
		void answer_in_turn(const std::string &node, const std::vector<router::response> &responses);

		// A node that takes a while to answer whatever it answers.
		void slow(const std::string &node, std::chrono::milliseconds delay);

		// The node ordering writes to this key, and the term it orders them in.
		void led_by(const std::string &key, const std::string &node, int64_t term);

		// A key whose partition is led by nobody yet, which is a write with nowhere to go.
		void led_by_nobody(const std::string &key);

		// A term this node has already applied a write of, so that anything older is refused.
		void applied(const std::string &key, int64_t term);

		// A node that has been unable to order a write for long enough to be taken out of the
		// load balancer.
		void unled();

		// The etcd this node reads the membership from, and that it is registered there. A test
		// that says nothing is an instance that was told no etcd at all.
		void reads_etcd(const std::string &endpoint);

		// The same etcd, no longer answering: the node is still looking there and no longer holds
		// the lease its registration is written on.
		void lost_etcd();

		// The membership is the one the test named, so there is nothing to join, nothing to read
		// and nothing to leave.
		void start() override;

		bool discover() override;

		void stop() override;

		std::vector<member> members() const override;

		placement replicas(const std::string &key) const override;

		partition_set holdings() const override;

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

		const std::vector<std::pair<std::string, router::request>> &sent() const;

		void forget();
	};
}

#endif
