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
#include "reconcile/reconcile.h"
#include "server.h"
#include "session.h"

namespace
{
	constexpr int threads_per_core = 8;

	// hardware_concurrency() is allowed to answer nothing, and a pool of nothing serves nothing.
	constexpr int fewest_threads = 16;

	// Each thread keeps its own curl handles, so a very large pool is more connections to every
	// neighbour than a neighbour wants. A node that needs more is told so with ASYNCDB_THREADS.
	constexpr int most_threads = 128;

	// A third of the lease, as the membership is read on, because reading it more often than it is
	// written is polling for an answer that cannot have changed.
	constexpr int reconcile_seconds = 3;

	// The membership as it stands against the one a node last acted on. Two readings naming the
	// same nodes in the same zones are the same membership: the order is etcd's own, which is name
	// order, so a difference here is a node that joined, left, or moved zone.
	bool same_membership(const std::vector<cluster::member> &before, const std::vector<cluster::member> &after)
	{
		if (before.size() != after.size())
		{
			return false;
		}

		for (size_t i = 0; i < before.size(); i++)
		{
			if (before[i].node != after[i].node || before[i].zone != after[i].zone)
			{
				return false;
			}
		}

		return true;
	}
}

std::string server::data_directory()
{
	const char *configured = getenv("ASYNCDB_DATA");

	return configured == NULL || *configured == '\0' ? "/var/lib/asyncdb" : configured;
}

size_t server::memory_size()
{
	const char *configured = getenv("ASYNCDB_MEMORY");
	size_t mebibytes = 0;

	if (configured != NULL &&
		boost::conversion::try_lexical_convert(std::string(configured), mebibytes) &&
		mebibytes > 0)
	{
		return mebibytes * 1024 * 1024;
	}

	return repository::default_memory_bytes;
}

int server::thread_pool_size()
{
	const char *configured = getenv("ASYNCDB_THREADS");
	int threads = 0;

	if (configured != NULL && boost::conversion::try_lexical_convert(std::string(configured), threads) && threads > 0)
	{
		return threads;
	}

	return std::clamp(
		static_cast<int>(std::thread::hardware_concurrency()) * threads_per_core,
		fewest_threads,
		most_threads);
}

std::chrono::seconds server::reconcile_interval()
{
	return std::chrono::seconds(reconcile_seconds);
}

server::server::server(
	boost::asio::ip::port_type port,
	int threads,
	cluster::cluster &cluster_nodes,
	const std::string &directory,
	size_t memory_bytes):
	thread_count(threads),
	io_context(thread_count),
	acceptor(boost::asio::make_strand(io_context)),
	repository(repository::rocksdb_repository(directory, memory_bytes)),
	nodes(cluster_nodes),
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

	// Listening is held back until serve() has filled the store: a bound socket that is not
	// listening refuses at once, where a listening one nothing accepts on takes the connection
	// and answers nothing.
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

server::server::~server()
{
	stop_reconciling();
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
	if (nodes.discover())
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

	nodes.start();

	// Only once this node is a member. A node that has not joined owns every key it is asked
	// about — replicas() answers a cluster of one — so a pass before this would find nothing to
	// fetch and nothing to clear down, and a pass part way through joining would find the wrong
	// thing to do about both.
	{
		std::lock_guard<std::mutex> lock(reconcile_mutex);

		reconciling = true;
	}

	reconciler = std::thread([this]() { reconcile(); });

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

void server::server::reconcile()
{
	std::unique_lock<std::mutex> lock(reconcile_mutex);

	// What this node starts on is not a change. A store that matches the membership it was left
	// with needs nothing done to it, and a node whose membership never moves — a test naming its
	// own cluster, an instance standing alone — never runs a pass at all.
	std::vector<cluster::member> seen = nodes.members();
	int attempts = 0;

	while (!reconcile_wake.wait_for(lock, reconcile_interval(), [this]() { return !reconciling; }))
	{
		lock.unlock();

		std::vector<cluster::member> now = nodes.members();

		if (!same_membership(seen, now))
		{
			DEBUG("The membership moved, so the records whose owner moved with it are next.");

			seen = now;
			attempts = reconcile_attempts;
		}
		else if (attempts > 0)
		{
			attempts--;

			// As best effort as the rebuild is, and for the same reason: a store that refuses a
			// write, or a neighbour that answers something unreadable, would otherwise take the
			// process down over records that are a copy of records elsewhere. A pass that threw
			// is a pass that did some of it, and the attempts left are what runs the rest.
			try
			{
				// A pass that is waiting on nothing has done everything this membership asks
				// for. One that is waiting keeps its remaining attempts, because what unblocks
				// it is another node's own pass rather than anything this one can do again.
				if (reconcile::reconcile(repository, nodes).settled())
				{
					attempts = 0;
				}
			}
			catch (const std::exception &caught)
			{
				DEBUG(std::string("A reconcile did not finish: ") + caught.what());
			}
			catch (...)
			{
				DEBUG("A reconcile did not finish.");
			}
		}

		lock.lock();
	}
}

void server::server::stop_reconciling()
{
	{
		std::lock_guard<std::mutex> lock(reconcile_mutex);

		if (!reconciling)
		{
			return;
		}

		reconciling = false;
	}

	reconcile_wake.notify_all();

	if (reconciler.joinable())
	{
		reconciler.join();
	}
}

void server::server::close()
{
	// Leaving the cluster before the acceptor is closed means the other nodes stop sending keys
	// here while this instance can still answer for the ones already in flight.
	nodes.stop();

	// A pass holds a thread and asks the other nodes questions, so it goes before the connections
	// it would ask them over do.
	stop_reconciling();

	std::vector<std::weak_ptr<session>> live;

	{
		std::lock_guard<std::mutex> lock(session_mutex);

		stopping = true;
		live = sessions;
	}

	// The acceptor belongs to a strand, so it is closed on that strand rather than on whichever
	// thread asked for it.
	boost::asio::dispatch(acceptor.get_executor(), [this]() { acceptor.close(); });

	for (size_t i = 0; i < live.size(); i++)
	{
		std::shared_ptr<session> connection = live[i].lock();

		if (connection)
		{
			connection->stop();
		}
	}
}
