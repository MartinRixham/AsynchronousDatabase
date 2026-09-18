#pragma once

#include <optional>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include "router/request.h"
#include "router/response.h"

namespace cluster
{
	constexpr char forwarded_header[] = "X-Asyncdb-Forwarded";

	constexpr char term_header[] = "X-Asyncdb-Term";

	// The count the leader stamped the write with. The term beside it is the other half of the
	// version, so this travels only where that does.
	constexpr char count_header[] = "X-Asyncdb-Count";

	// A request and the node it is for, so that several different ones can be asked at once.
	struct enquiry
	{
		std::string node;

		router::request request;
	};

	// The seam over asking another instance, in the way that cluster::cluster is the seam over
	// which instance to ask. They are two questions and not one: a request is routed by the
	// membership and sent over the network, and a caller that has worked out where a key lives
	// still has to get there.
	class forwarder
	{
	public:
		[[nodiscard]] virtual router::response forward(
			const std::string &node,
			const router::request &request) const = 0;

		// The same request to every node named, all of them at once, answered one for one and in
		// the order the nodes were named rather than the order they answered in. A node that did
		// not answer is an answer of its own, the way it is when it is asked on its own.
		//
		// A forwarder that can ask them at once asks them at once. A write is not done until every
		// copy has taken it, and asking one after another holds the thread serving the write for a
		// round trip each — and a thread waiting on another node cannot answer anything else, its
		// health check included.
		[[nodiscard]] virtual std::vector<router::response> forward_all(
			const std::vector<std::string> &nodes,
			const router::request &request) const = 0;

		// The same fan out for a caller asking each node something different, which is what a walk
		// of a share in several pieces at once is.
		[[nodiscard]] virtual std::vector<router::response> forward_each(
			const std::vector<enquiry> &enquiries) const = 0;

		// forward() on the executor of the coroutine awaiting it, which holds no thread while the
		// other node answers. The request has to outlive the wait.
		[[nodiscard]] virtual boost::asio::awaitable<router::response> async_forward(
			const std::string &node,
			const router::request &request) const = 0;
	};

	// The first answer of a fan out that refused, or nothing where every node took it. It is what a
	// write does with what forward_all() gives back: a copy that refused is the answer, and the
	// others are a write the refusing one was written beside rather than behind.
	std::optional<router::response> refusal(const std::vector<router::response> &answers);
}
