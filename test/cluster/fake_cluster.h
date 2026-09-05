#ifndef CLUSTER_FAKE_CLUSTER_H
#define CLUSTER_FAKE_CLUSTER_H

#include <map>
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

		std::map<std::string, leadership> leaders;

		std::map<std::string, int64_t> refused;

		mutable std::vector<std::pair<std::string, router::request>> requests;

	public:
		fake_cluster(const std::string &node, const std::vector<std::string> &members);

		fake_cluster(const std::string &node, const std::vector<member> &members);

		// The one node holding the key, which is a cluster keeping one copy.
		void owns(const std::string &key, const std::string &node);

		// Every node holding a copy of the key, in the order this node would ask them.
		void copies(const std::string &key, const std::vector<std::string> &nodes);

		void answer(const std::string &node, const router::response &response);

		// The node ordering writes to this key, and the term it orders them in.
		void led_by(const std::string &key, const std::string &node, int64_t term);

		// A key whose partition is led by nobody yet, which is a write with nowhere to go.
		void led_by_nobody(const std::string &key);

		// A term this node has already applied a write of, so that anything older is refused.
		void applied(const std::string &key, int64_t term);

		std::vector<member> members() const override;

		placement replicas(const std::string &key) const override;

		std::vector<std::string> peers() const override;

		std::vector<std::vector<std::string>> zones() const override;

		std::optional<leadership> leader(const std::string &key) const override;

		size_t leads() const override;

		bool accept(const std::string &key, int64_t term) override;

		router::response send(const std::string &node, const router::request &request) const override;

		const std::vector<std::pair<std::string, router::request>> &sent() const;

		void forget();
	};
}

#endif
