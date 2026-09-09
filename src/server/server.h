#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>

#include "cluster/cluster.h"
#include "router/router.h"
#include "repository/rocksdb_repository.h"

namespace server
{
	// Where the store is kept, from ASYNCDB_DATA. It is a directory of its own for each instance,
	// because RocksDB locks the one it opens: two instances in one process — which is two servers
	// in one test — are two directories.
	std::string data_directory();

	int thread_pool_size();

	// How often the membership is read to see whether it moved, which is how soon a node starts
	// moving the records whose owner changed. It is a poll rather than a signal because the
	// membership is a moment: acting on the first reading of a change is acting on a view that may
	// be a node registering or a lease that is about to be renewed, and the pass is deliberately
	// one tick behind for that reason.
	std::chrono::seconds reconcile_interval();

	// How many passes a membership change buys. A pass that finds records waiting on another node
	// runs again, because that node's own pass is what unblocks it, and this is the bound on
	// waiting for a node whose pass is never coming — the copy it was waiting on is simply kept.
	constexpr int reconcile_attempts = 12;

	class session;

	class server : public std::enable_shared_from_this<server>
	{
		int const thread_count;

		boost::asio::io_context io_context;

		boost::asio::ip::tcp::acceptor acceptor;

		boost::asio::ip::port_type port_number;

		repository::rocksdb_repository repository;

		// The other instances, when there are any: a key belongs to one of them, and a request
		// for a key this instance does not hold is answered by asking the one that does.
		cluster::cluster &nodes;

		router::router router;

		std::mutex session_mutex;

		std::vector<std::weak_ptr<session>> sessions;

		std::atomic<bool> stopping = false;

		// Moving the records whose owner changed is work of its own and not part of serving, so it
		// is a thread of its own — the one the membership is watched on.
		std::thread reconciler;

		std::mutex reconcile_mutex;

		std::condition_variable reconcile_wake;

		bool reconciling = false;

	public:
		// The cluster is handed in and never made here: this server joins and leaves whichever one
		// it was given, which is etcd's for the binary and its own membership for a test.
		server(
			boost::asio::ip::port_type port,
			int thread_count,
			cluster::cluster &nodes,
			const std::string &directory = data_directory());

		// A thread that is still joinable when it goes would take the process with it, so a server
		// that was never closed still stops watching the membership here.
		~server();

		void serve();

		void on_accept(boost::beast::error_code error, boost::asio::ip::tcp::socket socket);

		boost::asio::ip::port_type port() const;

		void close();

	private:
		void hold(const std::shared_ptr<session> &session);

		// Opening the port to connections, which is done once the store is filled and the node has
		// joined rather than when the socket is bound: a bound socket that is not listening yet
		// refuses at once, and being refused is what sends a health check or a neighbour somewhere
		// else instead of holding it until its timeout.
		void listen();

		void accept();

		// Watches the membership, and moves records when it moves. Started once this node has
		// joined, because a node that is not a member owns nothing and would clear down the store.
		void reconcile();

		void stop_reconciling();
	};
}

#endif
