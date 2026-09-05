#ifndef CLUSTER_MEMBER_H
#define CLUSTER_MEMBER_H

#include <string>

namespace cluster
{
	// A node as the rest of the cluster knows it: where it answers, and which availability zone it
	// stands in. A zone of nothing is a node that was never told which zone it is in, and every
	// such node is in that one zone together — which is a cluster keeping one copy of a record,
	// exactly as it did before there were zones at all.
	struct member
	{
		std::string node;

		std::string zone;
	};
}

#endif
