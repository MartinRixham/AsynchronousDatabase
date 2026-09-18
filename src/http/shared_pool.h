#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/execution_context.hpp>

#include "connection.h"

namespace http
{
	// The connections every thread of one io_context holds between them, for the transfers that run
	// on that io_context rather than on one of a thread's own. It is a service of the io_context
	// because a socket has to go before the io_context it was made on does, and a service is shut
	// down first.
	class shared_pool : public boost::asio::execution_context::service
	{
		// A coroutine hands its connections back from whichever thread of the io_context it
		// finished on.
		std::mutex mutex;

		std::vector<std::unique_ptr<connection>> idle;

	public:
		static boost::asio::execution_context::id id;

		explicit shared_pool(boost::asio::execution_context &context);

		// A connection made here is made on the executor of the request asking for it. What a
		// transfer on it completes through is that request's own executor whichever request made
		// it, so a connection a strand made is safe to hand to another strand.
		[[nodiscard]] std::unique_ptr<connection> take(
			const boost::asio::any_io_executor &executor,
			const std::string &host,
			const std::string &port);

		void keep(std::unique_ptr<connection> used);

	private:
		void shutdown() override;
	};
}
