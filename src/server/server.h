#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include <boost/asio.hpp>

#include <thread>

#include "listener.h"

class server
{
    int const thread_count;

    boost::asio::io_context context;

    std::shared_ptr<listener> listen;

public:
    server(
        const std::string &address,
        boost::asio::ip::port_type port,
        int thread_count = std::thread::hardware_concurrency());

    void serve();

    boost::asio::ip::port_type port();
};

#endif
