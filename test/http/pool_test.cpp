#include <chrono>
#include <exception>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "http/pool.h"

namespace
{
	// A connection out of the pool, opened the way a transfer opens one: a closed connection is
	// one the pool does not keep.
	std::unique_ptr<http::connection> opened(http::pool &held, const std::string &host, const std::string &port)
	{
		std::unique_ptr<http::connection> link = held.take(host, port);

		boost::asio::co_spawn(
			held.context(),
			link->connect(std::chrono::steady_clock::now() + std::chrono::seconds(5)),
			[](const std::exception_ptr &thrown)
			{
				if (thrown)
				{
					std::rethrow_exception(thrown);
				}
			});

		held.run();

		return link;
	}

	std::string node(int number)
	{
		return "127.0.0." + std::to_string(number);
	}
}

// A thread holds a connection to every node it forwards to, up to a ceiling, and the one it drops
// to make room is the one it has gone longest without asking for — not the one it opened first,
// which is the neighbour it may well be talking to most. Every address in 127.0.0.0/8 is this
// machine, and a listener that never accepts still completes the handshakes, so sixty five of them
// are sixty five nodes.
TEST(pool_test, drop_the_connection_asked_for_longest_ago_to_keep_another)
{
	boost::asio::io_context context;
	boost::asio::ip::tcp::acceptor acceptor(
		context,
		boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("0.0.0.0"), 0));
	std::string port = std::to_string(acceptor.local_endpoint().port());
	http::pool held;

	for (int number = 1; number <= 64; number++)
	{
		held.keep(opened(held, node(number), port));
	}

	// The first node is asked for again, so the second is now the one gone longest without it.
	held.keep(held.take(node(1), port));
	held.keep(opened(held, node(65), port));

	EXPECT_FALSE(held.take(node(2), port)->is_open());
	EXPECT_TRUE(held.take(node(1), port)->is_open());

	for (int number = 3; number <= 65; number++)
	{
		EXPECT_TRUE(held.take(node(number), port)->is_open()) << node(number);
	}
}
