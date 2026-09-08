#ifndef CLUSTER_MEMBER_H
#define CLUSTER_MEMBER_H

#include <memory>
#include <string>
#include <vector>

namespace cluster
{
	// A node as the rest of the cluster knows it: where it answers, and which availability zone it
	// stands in. A zone of nothing is a node that was never told which zone it is in, and every
	// such node is in that one zone together, which is a cluster keeping one copy of a record.
	struct member
	{
		std::string node;

		std::string zone;
	};

	// The whole membership, read as one thing. It is shared rather than copied and never edited
	// once it is published, so every node that reads it reads the same list and no reader has to
	// exclude the thread that replaces it.
	typedef std::shared_ptr<const std::vector<member>> membership;
}

#endif
