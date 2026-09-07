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

		// Every request at once, answered in the order they were given rather than the order they
		// finished in. A client with no way of running several at once runs them one after
		// another, which is what this does unless something overrides it.
		virtual std::vector<response> send_all(const std::vector<request> &requests) const;
	};

	class curl_client : public client
	{
		long timeout_seconds;

		// Connecting is bounded apart from the transfer, because the two answer different
		// questions. A node that is not there is told from a node that is slow: the first costs
		// this and the second costs the whole timeout, which has to be long enough to carry
		// sixteen megabytes.
		long connect_timeout_seconds;

	public:
		curl_client(long timeout, long connect_timeout);

		response send(const request &request) const override;

		std::vector<response> send_all(const std::vector<request> &requests) const override;
	};
}

#endif
