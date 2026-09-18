#pragma once

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

		// The other half of the version a write was stamped with, the term being the first.
		uint64_t count = 0;
	};
}
