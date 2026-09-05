#ifndef CLUSTER_CLUSTER_H
#define CLUSTER_CLUSTER_H

#include <optional>
#include <string>
#include <vector>

#include "router/request.h"
#include "router/response.h"
#include "member.h"

namespace cluster
{
	// A request that has already been sent to the node that owns its key. That node serves it
	// where it stands, so that two nodes that disagree about the membership for a moment cannot
	// bounce a request between them for ever.
	constexpr char forwarded_header[] = "X-Asyncdb-Forwarded";

	// Where the copies of a key are: whether this node holds one itself, and the other nodes that
	// hold one, in the order this node should ask them — its own zone first, because the copy in
	// this node's zone is the near one.
	struct placement
	{
		// True when this node holds the key, which is what an instance standing alone answers for
		// every key.
		bool local = true;

		std::vector<std::string> nodes;
	};

	// The seam over the other instances, in the way that repository::repository is the seam over
	// the store. A key belongs to one node in each zone, and a request for a key this node holds no
	// copy of is answered by a node that does.
	class cluster
	{
	public:
		// Every node including this one, in name order. Empty when this instance stands alone.
		virtual std::vector<member> members() const = 0;

		// Every node holding a copy of the key.
		virtual placement replicas(const std::string &key) const = 0;

		// Every node but this one.
		virtual std::vector<std::string> peers() const = 0;

		// Sends the request to the node named and returns its answer as this node's own. The node
		// serves it where it stands rather than forwarding it again.
		virtual router::response send(const std::string &node, const router::request &request) const = 0;
	};
}

#endif
