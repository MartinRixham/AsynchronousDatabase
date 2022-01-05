#include <memory>
#include <vector>

#include "listener.h"
#include "server.h"

server::server(
    const std::string &addr,
    boost::asio::ip::port_type prt,
    int threads):
        address(boost::asio::ip::make_address(addr)),
        port(prt),
        thread_count(threads)
{   
}

void server::serve()
{
    boost::asio::io_context io_context{thread_count};

    std::make_shared<listener>(
        io_context,
        boost::asio::ip::tcp::endpoint{address, port})->run();

    std::vector<std::thread> threads;

    threads.reserve(thread_count - 1);

    for (int i = 0; i < thread_count - 1; i++)
    {
        threads.emplace_back(
            [&io_context]()
            {
                io_context.run();
            });
    }

    io_context.run();
}
