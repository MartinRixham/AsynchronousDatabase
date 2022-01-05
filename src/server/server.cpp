#include <memory>
#include <vector>

#include "listener.h"
#include "server.h"

server::server::server(
    const std::string &address,
    boost::asio::ip::port_type port,
    int threads):
        thread_count(threads),
        context(thread_count),
        listen(std::shared_ptr<listener>(new listener(
            context, boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address(address), port})))
{   
}

void server::server::serve()
{
    listen->run();

    std::vector<std::thread> threads;

    threads.reserve(thread_count - 1);

    for (int i = 0; i < thread_count - 1; i++)
    {
        threads.emplace_back(
            [this]()
            {
                context.run();
            });
    }

    context.run();
}

boost::asio::ip::port_type server::server::port()
{
    return listen->port();
}
