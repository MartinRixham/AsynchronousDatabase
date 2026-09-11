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

		std::vector<std::string> path;

		std::string query;

		std::string body;

		bool forwarded = false;

		int64_t term = 0;

		// The other half of the version a write was stamped with, the term being the first. It
		// travels with a term for a record, and on its own for a schema operation ordered where
		// nothing leads: a node with no leadership to claim still stamps the operation once and
		// every node applies the stamp it was given.
		uint64_t count = 0;
	};
}

#endif
