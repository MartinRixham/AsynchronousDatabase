#ifndef CLUSTER_FORWARDER_H
#define CLUSTER_FORWARDER_H

#include <string>

#include "http/http_client.h"
#include "router/request.h"
#include "router/response.h"

namespace cluster
{
	// How a request travels to another node, apart from which node that is: the request is sent
	// as it stands, marked as forwarded so that the node it reaches serves it where it stands,
	// and the answer becomes this node's answer.
	router::response forward(const http::client &http, const std::string &node, const router::request &request);
}

#endif
