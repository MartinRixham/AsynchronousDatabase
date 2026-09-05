#ifndef CLUSTER_FORWARDER_H
#define CLUSTER_FORWARDER_H

#include <string>
#include <vector>

#include "http/http_client.h"
#include "router/request.h"
#include "router/response.h"

namespace cluster
{
	// How a request travels to another node, apart from which node that is: the request is sent
	// as it stands, marked as forwarded so that the node it reaches serves it where it stands,
	// and the answer becomes this node's answer.
	router::response forward(const http::client &http, const std::string &node, const router::request &request);

	// The same request to every node named, all of them at once, answered one for one and in the
	// order the nodes were named rather than the order they answered in. A node that did not
	// answer is an answer of its own, the way it is when it is asked on its own.
	std::vector<router::response> forward_all(
		const http::client &http,
		const std::vector<std::string> &nodes,
		const router::request &request);
}

#endif
