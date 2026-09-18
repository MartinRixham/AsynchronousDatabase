#include <poll.h>

#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/lexical_cast/try_lexical_convert.hpp>

#include "connection.h"

namespace
{
	// Beast reads off the socket no more than the buffer has room for, and the parser empties it
	// into the body as it goes, so a buffer left to grow on its own reads a body 512 bytes at a
	// time. This is the most Beast reads at once.
	constexpr size_t read_size = 64 * 1024;

	// A node closes a connection it has waited sixty seconds on for a request (server::session), and
	// a request sent on one it has closed can go unanswered until the unacknowledged timeout rather
	// than be refused. So a connection is given up well before the node would give it up.
	constexpr std::chrono::seconds max_idle(45);
}

http::connection::connection(
	const boost::asio::any_io_executor &executor,
	const std::string &host,
	const std::string &port):
	host_name(host),
	port_name(port),
	socket_stream(executor)
{
	received.reserve(read_size);
}

bool http::connection::goes_to(const std::string &host, const std::string &port) const noexcept
{
	return host_name == host && port_name == port;
}

bool http::connection::is_open() const noexcept
{
	return socket_stream.socket().is_open();
}

void http::connection::mark_idle() noexcept
{
	idle_since = std::chrono::steady_clock::now();
}

bool http::connection::is_reusable(std::chrono::steady_clock::time_point at) noexcept
{
	if (!is_open() || at - idle_since >= max_idle)
	{
		return false;
	}

	// Nothing is due on a connection between an answer and the next request, so anything to read
	// on one is the node closing it, or a reset.
	pollfd waiting = { socket_stream.socket().native_handle(), POLLIN, 0 };

	return poll(&waiting, 1, 0) == 0;
}

boost::beast::tcp_stream &http::connection::stream() noexcept
{
	return socket_stream;
}

boost::beast::flat_buffer &http::connection::buffer() noexcept
{
	return received;
}

boost::asio::awaitable<void> http::connection::connect(std::chrono::steady_clock::time_point until)
{
	boost::system::error_code not_an_address;
	boost::asio::ip::address address = boost::asio::ip::make_address(host_name, not_an_address);

	socket_stream.expires_at(until);

	// An address that is already one costs no lookup, and asks for no resolver: Asio answers a
	// name on a thread of the io_context's own, started the first time one is asked for.
	if (!not_an_address)
	{
		boost::asio::ip::port_type port = 0;

		if (!boost::conversion::try_lexical_convert(port_name, port))
		{
			throw boost::system::system_error(boost::asio::error::invalid_argument);
		}

		co_await socket_stream.async_connect(boost::asio::ip::tcp::endpoint(address, port), boost::asio::use_awaitable);

		co_return;
	}

	// A name is looked up once for the connection rather than once for the request.
	boost::asio::ip::tcp::resolver resolver(socket_stream.get_executor());
	boost::asio::ip::tcp::resolver::results_type addresses =
		co_await resolver.async_resolve(host_name, port_name, boost::asio::use_awaitable);

	co_await socket_stream.async_connect(addresses, boost::asio::use_awaitable);
}

void http::connection::close()
{
	boost::system::error_code ignored;

	socket_stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
	socket_stream.close();
	received.clear();
}
