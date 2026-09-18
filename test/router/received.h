#pragma once

#include "router/response.h"
#include "repository/bytes_of.h"

// A response as the node that asked for it receives one. A file the router answered with is on the
// disk of the node that sent it, and what crosses the network is its bytes.
inline router::response received(router::response sent)
{
	if (sent.file.sent)
	{
		sent.text = bytes_of(sent.file.sent);
		sent.file.sent = nullptr;
	}

	return sent;
}
