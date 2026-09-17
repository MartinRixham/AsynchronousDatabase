#include "connection.h"

http::connection::connection(boost::asio::io_context &context, const std::string &host, const std::string &port):
	where(host + ":" + port),
	host_name(host),
	port_name(port),
	socket_stream(context)
{
}

const std::string &http::connection::node() const noexcept
{
	return where;
}

bool http::connection::is_open() const noexcept
{
	return open;
}

boost::beast::tcp_stream &http::connection::stream() noexcept
{
	return socket_stream;
}

boost::beast::flat_buffer &http::connection::buffer() noexcept
{
	return received;
}

boost::asio::ip::tcp::resolver::results_type http::connection::addresses(boost::system::error_code &error)
{
	boost::asio::ip::tcp::resolver resolver(socket_stream.get_executor());

	return resolver.resolve(host_name, port_name, error);
}

void http::connection::opened() noexcept
{
	open = true;
}

void http::connection::close()
{
	boost::system::error_code ignored;

	socket_stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
	socket_stream.close();
	received.clear();

	open = false;
}
