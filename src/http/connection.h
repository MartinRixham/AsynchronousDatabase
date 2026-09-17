#pragma once

#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>

namespace http
{
	// One connection to one node, kept open between the requests that go there. It belongs to the
	// thread that made it and is never shared: two answers read into one buffer are neither.
	class connection
	{
		std::string where;

		std::string host_name;

		std::string port_name;

		boost::beast::tcp_stream socket_stream;

		boost::beast::flat_buffer received;

		bool open = false;

	public:
		connection(boost::asio::io_context &context, const std::string &host, const std::string &port);

		connection(const connection &) = delete;

		connection &operator=(const connection &) = delete;

		// The host and port together, which is what a connection is kept under.
		[[nodiscard]] const std::string &node() const noexcept;

		[[nodiscard]] bool is_open() const noexcept;

		[[nodiscard]] boost::beast::tcp_stream &stream() noexcept;

		// What was read off the socket and not yet parsed, which belongs to the connection and
		// not to the request: a keep alive answer can arrive in the same read as the end of the
		// one before it.
		[[nodiscard]] boost::beast::flat_buffer &buffer() noexcept;

		// The addresses this connection's host answers as. It is the thread that waits for them
		// rather than the io_context, because an address that is already one costs no lookup and
		// a name is looked up once for the connection rather than once for the request.
		[[nodiscard]] boost::asio::ip::tcp::resolver::results_type addresses(boost::system::error_code &error);

		void opened() noexcept;

		void close();
	};
}
