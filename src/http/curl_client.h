#pragma once

#include <vector>

#include "http_client.h"

namespace http
{
	class curl_client : public client
	{
		// Connecting is bounded apart from the transfer, because the two answer different
		// questions. A node that is not there is told from a node that is slow: the first costs
		// this and the second costs the whole timeout, which has to be long enough to carry
		// sixteen megabytes.
		long connect_timeout_seconds;

		// A node that went from the network after the connection was made — stopped, or cut off
		// — acknowledges nothing it is sent, where a node that is slow still does. So this is
		// what a connection that has gone dead costs, rather than the whole timeout.
		long unacknowledged_timeout_seconds;

	public:
		curl_client(long connect_timeout, long unacknowledged_timeout);

		[[nodiscard]] response send(const request &request, long timeout_seconds) const override;

		[[nodiscard]] std::vector<response> send_all(
			const std::vector<request> &requests,
			long timeout_seconds) const override;
	};
}
