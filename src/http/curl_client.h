#ifndef HTTP_CURL_CLIENT_H
#define HTTP_CURL_CLIENT_H

#include <vector>

#include "http_client.h"

namespace http
{
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

		response send(const request &request, long timeout_override) const override;

		std::vector<response> send_all(const std::vector<request> &requests) const override;
	};
}

#endif
