#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include <boost/asio.hpp>

#include <thread>

class server
{
    boost::asio::ip::address address;

    boost::asio::ip::port_type port;

    int const thread_count;

public:
    server(
        const std::string &addr,
        boost::asio::ip::port_type port,
        int thread_count = std::thread::hardware_concurrency());

    void serve();
};

#endif
