#ifndef CLUSTER_CLUSTER_H
#define CLUSTER_CLUSTER_H

#include <optional>
#include <string>
#include <vector>

#include "router/request.h"
#include "router/response.h"

namespace cluster
{
	// A request that has already been sent to the node that owns its key. That node serves it
	// where it stands, so that two nodes that disagree about the membership for a moment cannot
	// bounce a request between them for ever.
	constexpr char forwarded_header[] = "X-Asyncdb-Forwarded";

	// The seam over the other instances, in the way that repository::repository is the seam over
	// the store. A key belongs to exactly one node, and a request for a key this node does not own
	// is answered by the node that does.
	class cluster
	{
	public:
		// Every node including this one, in name order. Empty when this instance stands alone.
		virtual std::vector<std::string> members() const = 0;

		// The node that owns the key, or nothing at all when this node owns it — which is also
		// what an instance standing alone answers for every key.
		virtual std::optional<std::string> owner(const std::string &key) const = 0;

		// Every node but this one.
		virtual std::vector<std::string> peers() const = 0;

		// Sends the request to the node named and returns its answer as this node's own. The node
		// serves it where it stands rather than forwarding it again.
		virtual router::response send(const std::string &node, const router::request &request) const = 0;
	};
}

#endif
