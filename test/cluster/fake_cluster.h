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

		mutable std::vector<std::pair<std::string, router::request>> requests;

	public:
		fake_cluster(const std::string &node, const std::vector<std::string> &members);

		fake_cluster(const std::string &node, const std::vector<member> &members);

		// The one node holding the key, which is a cluster keeping one copy.
		void owns(const std::string &key, const std::string &node);

		// Every node holding a copy of the key, in the order this node would ask them.
		void copies(const std::string &key, const std::vector<std::string> &nodes);

		void answer(const std::string &node, const router::response &response);

		std::vector<member> members() const override;

		placement replicas(const std::string &key) const override;

		std::vector<std::string> peers() const override;

		std::vector<std::vector<std::string>> zones() const override;

		router::response send(const std::string &node, const router::request &request) const override;

		const std::vector<std::pair<std::string, router::request>> &sent() const;

		void forget();
	};
}

#endif
