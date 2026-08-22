#include <algorithm>
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
		server(port, threads, cluster::from_environment(), NULL)
{
}

server::server::server(
	boost::asio::ip::port_type port,
	int threads,
	cluster::cluster &cluster_nodes):
		server(port, threads, cluster::config(), &cluster_nodes)
{
}

server::server::server(
	boost::asio::ip::port_type port,
	int threads,
	const cluster::config &configuration,
	cluster::cluster *external):
		thread_count(threads),
		io_context(thread_count),
		acceptor(boost::asio::make_strand(io_context)),
		repository(repository::rocksdb_repository("/tmp/asyncdb")),
		own_nodes(cluster::etcd_cluster(configuration)),
		nodes(external == NULL ? static_cast<cluster::cluster &>(own_nodes) : *external),
		router(router::router(repository, nodes))
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
	// Joining is what makes this instance one of several, and it is nothing at all when no etcd
	// is configured, which is how a single instance keeps the whole keyspace to itself.
	own_nodes.start();

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
		// Closing the acceptor is how serving is stopped, so an accept that ends this way has
		// ended because it was meant to, and saying "error" about it is how a clean shutdown
		// comes to look like a failure. Accepting again would spin on the same error, and an
		// accept that is always pending holds a reference to the server, so neither it nor
		// anything it owns would ever be destroyed.
		if (!acceptor.is_open())
		{
			DEBUG("Stopped accepting connections.");

			return;
		}

		DEBUG("Error accepting a connection: " + error.message());
	}
	else
	{
		std::shared_ptr<session> connection = std::make_shared<session>(std::move(socket), router, stopping);

		hold(connection);
		connection->run();
	}

	accept();
}

boost::asio::ip::port_type server::server::port() const
{
	return port_number;
}

void server::server::hold(const std::shared_ptr<session> &connection)
{
	std::lock_guard<std::mutex> lock(session_mutex);

	// The connections that have ended are forgotten here rather than by anything of their own,
	// which keeps the cost of remembering them to the connections that are still open.
	sessions.erase(
		std::remove_if(
			sessions.begin(),
			sessions.end(),
			[](const std::weak_ptr<session> &held) { return held.expired(); }),
		sessions.end());

	sessions.push_back(connection);

	// A connection accepted as the server was being shut down is one close() has already walked
	// past, so it is told here instead.
	if (stopping)
	{
		connection->stop();
	}
}

void server::server::accept()
{
	acceptor.async_accept(
		boost::asio::make_strand(io_context),
		boost::beast::bind_front_handler(&server::on_accept, shared_from_this()));
}

void server::server::close()
{
	// Leaving the cluster before the acceptor is closed means the other nodes stop sending keys
	// here while this instance can still answer for the ones already in flight.
	own_nodes.stop();

	std::vector<std::weak_ptr<session>> live;

	{
		std::lock_guard<std::mutex> lock(session_mutex);

		stopping = true;
		live = sessions;
	}

	// The acceptor belongs to a strand, so it is closed on that strand rather than on whichever
	// thread asked for it.
	boost::asio::dispatch(acceptor.get_executor(), [this]() { acceptor.close(); });

	// Closing the acceptor stops connections being made, not connections that were made already.
	// A neighbour keeps its connection to this node open on purpose, and so does the nginx in
	// front of it, so a connection waiting for a request that is not coming would hold serving
	// open until it timed out.
	for (size_t i = 0; i < live.size(); i++)
	{
		std::shared_ptr<session> connection = live[i].lock();

		if (connection)
		{
			connection->stop();
		}
	}
}
