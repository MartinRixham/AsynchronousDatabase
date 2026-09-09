#ifndef HTTP_HTTP_CLIENT_H
#define HTTP_HTTP_CLIENT_H

#include <string>
#include <utility>
#include <vector>

namespace http
{
	// The only response headers read back off an answer. Everything this API says in a header of
	// its own is named this way, and a forwarded record write would otherwise pay for a copy of
	// every header a proxy in the path had put on it.
	constexpr char header_prefix[] = "X-Asyncdb-";

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

		std::vector<std::pair<std::string, std::string>> headers;
	};

	// What the answer carried under this name, or nothing. Header names are compared without
	// regard to case, because what a name arrives as is the sender's choice and not this one's.
	std::string header_of(const response &response, const std::string &name);

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
