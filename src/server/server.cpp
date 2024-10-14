#include <memory>
#include <vector>
#include <utility>

#include "error.h"
#include "log.h"
#include "server.h"
#include "session.h"

server::server::server(
	boost::asio::ip::port_type port,
	int threads):
		thread_count(threads),
		io_context(thread_count),
		acceptor(boost::asio::make_strand(io_context)),
		repository(repository::rocksdb_repository()),
		router(router::router(repository))
{
	boost::beast::error_code error;
	boost::asio::ip::tcp::endpoint endpoint { boost::asio::ip::address_v4::any(), port };

	acceptor.open(endpoint.protocol(), error);
	acceptor.set_option(boost::asio::socket_base::reuse_address(true), error);
	acceptor.bind(endpoint, error);

	if (error)
	{
		throw std::runtime_error(ERROR("Error binding to socket: " + error.message()));
	}

	acceptor.listen(boost::asio::socket_base::max_listen_connections, error);

	if (error)
	{
		throw std::runtime_error(ERROR("Error listening on socket: " + error.message()));
	}

	port_number = acceptor.local_endpoint().port();

	DEBUG("Server started on port: " + std::to_string(port_number) + ".");
}

void server::server::serve()
{
	accept();

	std::vector<std::thread> threads;

	threads.reserve(thread_count - 1);

	for (int i = 0; i < thread_count - 1; i++)
	{
		threads.emplace_back([this]() { io_context.run(); });
	}

	io_context.run();
}

void server::server::on_accept(boost::beast::error_code error, boost::asio::ip::tcp::socket socket)
{
	if (error)
	{
		DEBUG("Error reading from socket: " + error.message());
	}
	else
	{
		std::make_shared<session>(std::move(socket), router)->run();
	}

	accept();
}

boost::asio::ip::port_type server::server::port() const
{
	return port_number;
}

void server::server::accept()
{
	acceptor.async_accept(
		boost::asio::make_strand(io_context),
		boost::beast::bind_front_handler(&server::on_accept, shared_from_this()));
}

void server::server::close()
{
	acceptor.close();
}
