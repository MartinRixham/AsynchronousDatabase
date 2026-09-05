#ifndef CLUSTER_STANDALONE_H
#define CLUSTER_STANDALONE_H

#include "cluster.h"

namespace cluster
{
	// One instance owning the whole keyspace, which is what asyncdb is when no etcd is configured
	// and what the router is given when it is constructed without a cluster.
	class standalone : public cluster
	{
	public:
		std::vector<member> members() const override;

		placement replicas(const std::string &key) const override;

		std::vector<std::string> peers() const override;

		std::vector<std::vector<std::string>> zones() const override;

		router::response send(const std::string &node, const router::request &request) const override;
	};
}

#endif
