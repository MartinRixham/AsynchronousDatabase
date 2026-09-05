#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rocksdb/db.h>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>

#include "cluster/etcd_cluster.h"
#include "router/router.h"
#include "repository/rocksdb_repository.h"

namespace server
{
	// Where the store is kept, from ASYNCDB_DATA: one directory, which an instance opens and
	// opens again when it is started again, so a node that comes back holds what it wrote. Set
	// nothing and it is the directory the image mounts a volume over.
	//
	// It is a directory of its own for each instance, because RocksDB locks the one it opens: two
	// instances in one process — which is two servers in one test — are two directories.
	std::string data_directory();

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
		cluster::etcd_cluster own_nodes;

		cluster::cluster &nodes;

		router::router router;

		// Serving ends when the acceptor is closed and every connection has gone, and a connection
		// that is kept alive between requests goes when it is told to. The sessions are held
		// weakly: a connection that ends first is gone from here by having ended.
		std::mutex session_mutex;

		std::vector<std::weak_ptr<session>> sessions;

		std::atomic<bool> stopping = false;

	public:
		explicit server(
			boost::asio::ip::port_type port,
			int thread_count = std::thread::hardware_concurrency(),
			const std::string &directory = data_directory());

		// The cluster a test names itself, rather than the one etcd names. Nothing is registered
		// and nothing is renewed: the membership is what it was given.
		server(
			boost::asio::ip::port_type port,
			int thread_count,
			cluster::cluster &nodes,
			const std::string &directory = data_directory());

		void serve();

		void on_accept(boost::beast::error_code error, boost::asio::ip::tcp::socket socket);

		boost::asio::ip::port_type port() const;

		void close();

	private:
		void hold(const std::shared_ptr<session> &session);

		server(
			boost::asio::ip::port_type port,
			int thread_count,
			const cluster::config &configuration,
			cluster::cluster *external,
			const std::string &directory);

		void accept();
	};
}

#endif
