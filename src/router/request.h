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

		// The other half of the version the leader stamped a write with, the term being the
		// first. It travels only with a term, because only a leader issues one.
		uint64_t count = 0;
	};
}

#endif
