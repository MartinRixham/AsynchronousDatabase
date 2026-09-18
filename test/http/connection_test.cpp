#include <chrono>
#include <exception>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast/core/error.hpp>
#include <gtest/gtest.h>

#include "http/connection.h"

// A node gone from the network answers a connect with nothing at all, which is not a refusal and
// would otherwise cost the whole timeout of the request. A listener with a backlog of none holds
// one connection nobody accepts, and Linux drops every handshake past it, which is that silence.
TEST(connection_test, give_up_connecting_at_the_time_named)
{
	boost::asio::io_context context;
	boost::asio::ip::tcp::acceptor acceptor(context);
	boost::asio::ip::tcp::endpoint local(boost::asio::ip::make_address("127.0.0.1"), 0);

	acceptor.open(local.protocol());
	acceptor.bind(local);
	acceptor.listen(0);

	boost::asio::ip::tcp::socket filling(context);

	filling.connect(acceptor.local_endpoint());

	http::connection link(context.get_executor(), "127.0.0.1", std::to_string(acceptor.local_endpoint().port()));
	std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	std::exception_ptr thrown;

	boost::asio::co_spawn(
		context,
		link.connect(started + std::chrono::milliseconds(200)),
		[&thrown](const std::exception_ptr &failed) { thrown = failed; });

	context.run();

	std::chrono::steady_clock::duration taken = std::chrono::steady_clock::now() - started;

	ASSERT_TRUE(thrown);

	try
	{
		std::rethrow_exception(thrown);
	}
	catch (const boost::system::system_error &error)
	{
		EXPECT_EQ(error.code(), boost::beast::error::timeout);
	}

	EXPECT_LT(taken, std::chrono::seconds(2));
}

TEST(connection_test, keep_room_for_a_whole_read)
{
	boost::asio::io_context context;
	http::connection link(context.get_executor(), "127.0.0.1", "80");

	EXPECT_GE(link.buffer().capacity(), 64 * 1024);

	link.close();

	EXPECT_GE(link.buffer().capacity(), 64 * 1024);
}
