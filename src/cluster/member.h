#ifndef CLUSTER_MEMBER_H
#define CLUSTER_MEMBER_H

#include <memory>
#include <string>
#include <vector>

namespace cluster
{
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
