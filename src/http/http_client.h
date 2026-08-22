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
		// False when the request never got an answer at all: a node that is gone is told from a
		// node that answered with an error.
		bool is_valid = false;

		long status = 0;

		std::string content_type;

		std::string body;

		std::string message;

		// True when the request went over a connection that was already open. Nothing branches on
		// it — it is how a test tells that connections are being kept rather than remade.
		bool reused = false;
	};

	// The seam over libcurl, so that a cluster can be driven in a test without a network.
	class client
	{
	public:
		virtual response send(const request &request) const = 0;
	};

	class curl_client : public client
	{
		long timeout_seconds;

	public:
		explicit curl_client(long timeout);

		response send(const request &request) const override;
	};
}

#endif
