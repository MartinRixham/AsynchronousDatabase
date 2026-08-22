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
		repository(repository::rocksdb_repository("/tmp/asyncdb")),
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

	// Serving ends when the acceptor is closed, and a thread that is still joinable when its
	// vector goes would terminate the process, so the others are waited for here.
	for (std::thread &thread : threads)
	{
		thread.join();
	}
}

void server::server::on_accept(boost::beast::error_code error, boost::asio::ip::tcp::socket socket)
{
	if (error)
	{
		DEBUG("Error reading from socket: " + error.message());

		// Closing the acceptor is how serving is stopped. Accepting again would spin on the same
		// error, and an accept that is always pending holds a reference to the server, so neither
		// it nor anything it owns would ever be destroyed.
		if (!acceptor.is_open())
		{
			return;
		}
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
	// The acceptor belongs to a strand, so it is closed on that strand rather than on whichever
	// thread asked for it.
	boost::asio::dispatch(acceptor.get_executor(), [this]() { acceptor.close(); });
}
