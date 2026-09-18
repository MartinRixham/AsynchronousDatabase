#pragma once

#include <chrono>
#include <string>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>

namespace http
{
	// One connection to one node, kept open between the requests that go there. It belongs to one
	// request at a time and is never shared: two answers read into one buffer are neither.
	class connection
	{
		std::string host_name;

		std::string port_name;

		boost::beast::tcp_stream socket_stream;

		boost::beast::flat_buffer received;

		std::chrono::steady_clock::time_point idle_since;

	public:
		connection(const boost::asio::any_io_executor &executor, const std::string &host, const std::string &port);

		connection(const connection &) = delete;

		connection &operator=(const connection &) = delete;

		[[nodiscard]] bool goes_to(const std::string &host, const std::string &port) const noexcept;

		[[nodiscard]] bool is_open() const noexcept;

		// Handed back to a pool, and waiting from now for the next request to the node.
		void mark_idle() noexcept;

		// Whether a connection that has been idle can still be sent a request at the time named:
		// open, closed by nothing at the other end, and not idle for as long as a node keeps one.
		[[nodiscard]] bool is_reusable(std::chrono::steady_clock::time_point at) noexcept;

		[[nodiscard]] boost::beast::tcp_stream &stream() noexcept;

		// What was read off the socket and not yet parsed, which belongs to the connection and
		// not to the request: a keep alive answer can arrive in the same read as the end of the
		// one before it.
		[[nodiscard]] boost::beast::flat_buffer &buffer() noexcept;

		// Throws what stopped it, a connection given up on at the time named included.
		boost::asio::awaitable<void> connect(std::chrono::steady_clock::time_point until);

		void close();
	};
}
