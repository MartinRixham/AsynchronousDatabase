#include <algorithm>
#include <memory>
#include <thread>
#include <vector>
#include <utility>

#include <stdlib.h>

#include <boost/lexical_cast/try_lexical_convert.hpp>

#include "error.h"
#include "log.h"
#include "rebuild/rebuild.h"
#include "server.h"
#include "session.h"

namespace
{
	// A thread waiting on a neighbour is doing no work here, so the pool counts requests that can
	// be in flight rather than cores.
	constexpr int threads_per_core = 8;

	// hardware_concurrency() is allowed to answer nothing, and a pool of nothing serves nothing.
	constexpr int fewest_threads = 16;

	// Each thread keeps its own curl handles, so a very large pool is more connections to every
	// neighbour than a neighbour wants. A node that needs more is told so with ASYNCDB_THREADS.
	constexpr int most_threads = 128;
}

std::string server::data_directory()
{
	const char *configured = getenv("ASYNCDB_DATA");

	// The image mounts a volume here, so an instance that is started again opens the store the one
	// before it wrote rather than an empty one.
	return configured == NULL || *configured == '\0' ? "/var/lib/asyncdb" : configured;
}

int server::thread_pool_size()
{
	const char *configured = getenv("ASYNCDB_THREADS");
	int threads = 0;

	// Something that is not a number, or is not a count of threads, is nothing configured.
	if (configured != NULL &&
		boost::conversion::try_lexical_convert(std::string(configured), threads) &&
		threads > 0)
	{
		return threads;
	}

	return std::clamp(
		static_cast<int>(std::thread::hardware_concurrency()) * threads_per_core,
		fewest_threads,
		most_threads);
}

server::server::server(
	boost::asio::ip::port_type port,
	int threads,
	const std::string &directory):
		server(port, threads, cluster::from_environment(), NULL, directory)
{
}

server::server::server(
	boost::asio::ip::port_type port,
	int threads,
	cluster::cluster &cluster_nodes,
	const std::string &directory):
		server(port, threads, cluster::config(), &cluster_nodes, directory)
{
}

server::server::server(
	boost::asio::ip::port_type port,
	int threads,
	const cluster::config &configuration,
	cluster::cluster *external,
	const std::string &directory):
		thread_count(threads),
		io_context(thread_count),
		acceptor(boost::asio::make_strand(io_context)),
		repository(repository::rocksdb_repository(directory)),
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

	// Binding settles the port, so a server constructed on port 0 can be asked which one it took
	// before it serves. Listening is held back until serve() has filled the store: a bound socket
	// that is not listening refuses at once, where a listening one nothing accepts on takes the
	// connection and answers nothing.
	port_number = acceptor.local_endpoint().port();

	DEBUG("Server bound to port: " + std::to_string(port_number) + ".");
}

void server::server::listen()
{
	boost::beast::error_code error;

	acceptor.listen(boost::asio::socket_base::max_listen_connections, error);

	if (error)
	{
		throw std::runtime_error(ERROR("Error listening on socket: " + error.message()));
	}

	DEBUG("Server listening on port: " + std::to_string(port_number) + ".");
}

void server::server::serve()
{
	// A node that came back empty is filled from a zone that still holds its records, and filled
	// *before* it registers: a node that is not registered is nobody's copy, so this holds up no
	// write, where registering first would make every write to its partitions wait for it.
	//
	// The nodes a rebuild finds are the ones already registered, so a cluster starting together
	// does not wait on itself. read_members() puts this node back whatever etcd says, which is
	// what makes "will I own this key?" answerable before joining.
	//
	// It is best effort: a store that refuses a write, or a neighbour that answers something
	// unreadable, would otherwise take the process down before it ever registered, and again on
	// every restart. A node that starts thin is a copy the cluster has.
	if (own_nodes.discover())
	{
		try
		{
			rebuild::rebuild(repository, nodes);
		}
		catch (const std::exception &caught)
		{
			DEBUG(std::string("The rebuild did not finish: ") + caught.what());
		}
		catch (...)
		{
			DEBUG("The rebuild did not finish.");
		}
	}

	// Joining is nothing at all when no etcd is configured, which is how a single instance keeps
	// the whole keyspace to itself.
	own_nodes.start();

	// The store is filled and the node has joined, so the port is opened to the connections that
	// were refused while it was not ready to answer them.
	listen();

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
		// The acceptor was closed, so this accept ended because it was meant to. Accepting again
		// would spin on the same error, and an always-pending accept holds a reference to the
		// server, so nothing it owns would ever be destroyed.
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

	// Connections that have ended are forgotten here, which keeps the cost of remembering them to
	// the ones still open.
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

	// Closing the acceptor stops connections being made, not connections already made. A neighbour
	// and the nginx in front of this node both keep theirs open, so a connection waiting for a
	// request that is not coming would hold serving open until it timed out.
	for (size_t i = 0; i < live.size(); i++)
	{
		std::shared_ptr<session> connection = live[i].lock();

		if (connection)
		{
			connection->stop();
		}
	}
}
