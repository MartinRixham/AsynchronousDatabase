#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "connection.h"

namespace
{
	// Beast reads off the socket no more than the buffer has room for, and the parser empties it
	// into the body as it goes, so a buffer left to grow on its own reads a body 512 bytes at a
	// time. This is the most Beast reads at once.
	constexpr size_t read_size = 64 * 1024;
}

http::connection::connection(boost::asio::io_context &context, const std::string &host, const std::string &port):
	host_name(host),
	port_name(port),
	socket_stream(context)
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
	// It is the thread that waits for the addresses rather than the io_context, because an address
	// that is already one costs no lookup and a name is looked up once for the connection rather
	// than once for the request.
	boost::asio::ip::tcp::resolver resolver(socket_stream.get_executor());
	boost::asio::ip::tcp::resolver::results_type addresses = resolver.resolve(host_name, port_name);

	socket_stream.expires_at(until);

	co_await socket_stream.async_connect(addresses, boost::asio::use_awaitable);
}

void http::connection::close()
{
	boost::system::error_code ignored;

	socket_stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
	socket_stream.close();
	received.clear();
}
