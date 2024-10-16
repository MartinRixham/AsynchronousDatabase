#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include <thread>

#include <rocksdb/db.h>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>

#include "router/router.h"
#include "repository/rocksdb_repository.h"

namespace server
{
	class server : public std::enable_shared_from_this<server>
	{
		int const thread_count;

		boost::asio::io_context io_context;

		boost::asio::ip::tcp::acceptor acceptor;

		boost::asio::ip::port_type port_number;

		repository::rocksdb_repository repository;

		router::router router;

	public:
		explicit server(
			boost::asio::ip::port_type port,
			int thread_count = std::thread::hardware_concurrency());

		void serve();

		void on_accept(boost::beast::error_code error, boost::asio::ip::tcp::socket socket);

		boost::asio::ip::port_type port() const;

		void close();

	private:
		void accept();
	};
}

#endif
