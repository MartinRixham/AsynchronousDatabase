#pragma once

#include <expected>
#include <functional>
#include <stop_token>
#include <string_view>
#include <vector>

#include "http_client.h"

namespace http
{
	class beast_client final : public client
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
		beast_client(long connect_timeout, long unacknowledged_timeout);

		[[nodiscard]] std::expected<response, std::string> send(const request &request, long timeout_seconds)
			const override;

		[[nodiscard]] std::vector<std::expected<response, std::string>> send_all(
			const std::vector<request> &requests,
			long timeout_seconds) const override;

		[[nodiscard]] boost::asio::awaitable<std::expected<response, std::string>> async_send(
			const request &request,
			long timeout_seconds) const override;

		[[nodiscard]] std::expected<response, std::string> stream(
			const request &request,
			const std::function<bool(std::string_view)> &receive,
			const std::stop_token &stop) const override;
	};
}
