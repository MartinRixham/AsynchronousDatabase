#ifndef CLUSTER_FORWARDER_H
#define CLUSTER_FORWARDER_H

#include <string>
#include <vector>

#include "http/http_client.h"
#include "router/request.h"
#include "router/response.h"
#include "cluster.h"

namespace cluster
{
	// How a request travels to the node that holds the key, and how what that node answers is read
	// back as an answer of this one's.
	class forwarder
	{
		const http::client &http_client;

		long timout_seconds;

	public:
		explicit forwarder(const http::client &http, long timeout_seconds = 30);

		router::response forward(const std::string &node, const router::request &request) const;

		// The same request to every node named, all of them at once, answered one for one and in the
		// order the nodes were named rather than the order they answered in. A node that did not
		// answer is an answer of its own, the way it is when it is asked on its own.
		std::vector<router::response> forward_all(const std::vector<std::string> &nodes, const router::request &request)
			const;

		// The same fan out for a caller asking each node something different, which is what a walk
		// of a share in several pieces at once is.
		std::vector<router::response> forward_each(const std::vector<enquiry> &enquiries) const;
	};
}

#endif
