#pragma once

#include <expected>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

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
		long status = 0;

		std::string content_type;

		std::string body;

		long content_length = 0;

		bool reused = false;

		std::vector<std::pair<std::string, std::string>> headers;
	};

	// What the answer carried under this name, or nothing. Header names are compared without
	// regard to case, because what a name arrives as is the sender's choice and not this one's.
	std::string header_of(const response &response, const std::string &name);

	// The seam over the network, so that a cluster can be driven in a test without one.
	class client
	{
	public:
		[[nodiscard]] virtual std::expected<response, std::string> send(const request &request, long timeout_seconds)
			const = 0;

		// The fan out: the caller waits for the slowest of the requests rather than for the sum
		// of them, so a client that can run them at once runs them at once.
		[[nodiscard]] virtual std::vector<std::expected<response, std::string>> send_all(
			const std::vector<request> &requests,
			long timeout_seconds) const = 0;

		// send() on the executor of the coroutine awaiting it, which holds no thread while it waits.
		// The request has to outlive the wait.
		[[nodiscard]] virtual boost::asio::awaitable<std::expected<response, std::string>> async_send(
			const request &request,
			long timeout_seconds) const = 0;

		// An answer that does not end on its own. Each piece of the body is handed to receive as it
		// arrives rather than kept, and the transfer runs until the server ends it, the connection
		// fails, receive answers false or stop is asked for — so it has no timeout but connecting.
		[[nodiscard]] virtual std::expected<response, std::string> stream(
			const request &request,
			const std::function<bool(std::string_view)> &receive,
			const std::stop_token &stop) const = 0;
	};
}
