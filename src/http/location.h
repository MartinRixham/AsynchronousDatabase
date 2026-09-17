#pragma once

#include <string>

namespace http
{
	// A URL as the pieces a request is sent with: a host to connect to and a target to ask that
	// host for, which is what Beast is given — it parses no URL of its own. Everything here is
	// spoken over http, so anything else is a location that is not valid rather than one to guess
	// at.
	struct location
	{
		bool is_valid = false;

		std::string host;

		std::string port;

		// The path and the query as they arrived. What a segment holds was encoded where the
		// request was built, and is not this layer's to touch.
		std::string target;

		// What the request names itself as going to, the port included wherever there is one.
		std::string authority;
	};

	location locate(const std::string &url);
}
