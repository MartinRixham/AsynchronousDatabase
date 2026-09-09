#ifndef HTTP_HTTP_CLIENT_H
#define HTTP_HTTP_CLIENT_H

#include <string>
#include <vector>

namespace http
{
	struct request
	{
		std::string method;

		std::string url;

		std::string body;

		std::vector<std::string> headers;
	};

	struct response
	{
		bool is_valid = false;

		long status = 0;

		std::string content_type;

		std::string body;

		long content_length = 0;

		std::string message;

		bool reused = false;
	};

	// The seam over libcurl, so that a cluster can be driven in a test without a network.
	class client
	{
	public:
		virtual response send(const request &request, long timeout_seconds) const = 0;

		// The fan out: the caller waits for the slowest of the requests rather than for the sum
		// of them, so a client that can run them at once runs them at once.
		virtual std::vector<response> send_all(const std::vector<request> &requests, long timeout_seconds) const = 0;
	};
}

#endif
