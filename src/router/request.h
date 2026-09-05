#ifndef ROUTER_REQUEST_H
#define ROUTER_REQUEST_H

#include <cstdint>
#include <string>
#include <vector>

#include <boost/beast/http.hpp>

namespace router
{
	struct request
	{
		boost::beast::http::verb method;

		// The path split at its unencoded slashes and then decoded, so that a key containing a
		// slash is one segment rather than two.
		std::vector<std::string> path;

		std::string query;

		std::string body;

		// True when another node sent this request here because this node owns the key. It is
		// served where it stands: it is neither forwarded again nor broadcast.
		bool forwarded = false;

		// The term the leader of the key's partition ordered this write in, and nothing at all
		// when no leader ordered it.
		int64_t term = 0;
	};
}

#endif
