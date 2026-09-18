#pragma once

#include <string>
#include <vector>

#include "http/http_client.h"
#include "router/request.h"
#include "router/response.h"
#include "forwarder.h"

namespace cluster
{
	// How a request travels to the node that holds the key, and how what that node answers is read
	// back as an answer of this one's.
	class http_forwarder : public forwarder
	{
		const http::client &http_client;

		long timeout_seconds;

	public:
		explicit http_forwarder(const http::client &http, long timeout = 30);

		[[nodiscard]] router::response forward(
			const std::string &node,
			const router::request &request) const override;

		[[nodiscard]] std::vector<router::response> forward_all(
			const std::vector<std::string> &nodes,
			const router::request &request) const override;

		[[nodiscard]] std::vector<router::response> forward_each(
			const std::vector<enquiry> &enquiries) const override;

		[[nodiscard]] boost::asio::awaitable<router::response> async_forward(
			const std::string &node,
			const router::request &request) const override;
	};
}
