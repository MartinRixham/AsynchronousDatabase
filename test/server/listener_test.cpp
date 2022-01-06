#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <boost/asio.hpp>

#include "server/listener.h"

TEST(listener_test, wrong_ip)
{
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::endpoint endpoint{boost::asio::ip::make_address("1.1.1.1"), 33333};

    std::string error;

    try
    {
        server::listener listener(io_context, endpoint);
    }
    catch(const std::exception &e)
    {
        error = e.what();
    }

    EXPECT_THAT(error, testing::StartsWith("src/server/listener.cpp"));
}

TEST(listener_test, multiple_connections)
{
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::endpoint endpoint{boost::asio::ip::make_address("127.0.0.1"), 33333};

    std::string error;

    try
    {
        server::listener listener1(io_context, endpoint);
        server::listener listener2(io_context, endpoint);
    }
    catch(const std::exception &e)
    {
        error = e.what();
    }

    EXPECT_THAT(error, testing::StartsWith("src/server/listener.cpp"));
}
