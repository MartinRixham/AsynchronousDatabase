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
	};
}

#endif
