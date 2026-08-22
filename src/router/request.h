#ifndef ROUTER_REQUEST_H
#define ROUTER_REQUEST_H

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
	};
}

#endif
