#pragma once

#include <memory>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>

#include "connection.h"

namespace http
{
	// The connections one thread holds, and the io_context they run on. A request takes the
	// connection to the node it is going to, or makes one where the thread has none to spare, and
	// hands it back once it has been answered — so a fan out of several requests to one node is a
	// connection for each of them, and the fan out after it is those same connections again.
	class pool
	{
		// The thread's own, and three descriptors of its own once a socket has been opened on it,
		// so a large thread pool is descriptors as well as connections.
		boost::asio::io_context io;

		std::vector<std::unique_ptr<connection>> idle;

	public:
		pool() = default;

		pool(const pool &) = delete;

		pool &operator=(const pool &) = delete;

		[[nodiscard]] boost::asio::io_context &context() noexcept;

		[[nodiscard]] std::unique_ptr<connection> take(const std::string &host, const std::string &port);

		void keep(std::unique_ptr<connection> used);

		// Every transfer this thread has started, until none of them is still going.
		void run();
	};
}
