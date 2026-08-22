#ifndef CLUSTER_FAKE_CLUSTER_H
#define CLUSTER_FAKE_CLUSTER_H

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cluster/cluster.h"

namespace cluster
{
	// A cluster of nodes that are not there: a key belongs to whichever node the test says, and a
	// node answers what the test told it to answer.
	class fake_cluster : public cluster
	{
		std::string self;

		std::vector<std::string> member_list;

		std::map<std::string, std::string> owners;

		std::map<std::string, router::response> answers;

		mutable std::vector<std::pair<std::string, router::request>> requests;

	public:
		fake_cluster(const std::string &node, const std::vector<std::string> &members);

		void owns(const std::string &key, const std::string &node);

		void answer(const std::string &node, const router::response &response);

		std::vector<std::string> members() const override;

		std::optional<std::string> owner(const std::string &key) const override;

		std::vector<std::string> peers() const override;

		router::response send(const std::string &node, const router::request &request) const override;

		const std::vector<std::pair<std::string, router::request>> &sent() const;

		void forget();
	};
}

#endif
