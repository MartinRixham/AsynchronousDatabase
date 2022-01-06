#include <memory>
#include <vector>

#include "listener.h"
#include "server.h"

server::server::server(
	boost::asio::ip::port_type port,
	int threads):
		thread_count(threads),
		io_context(thread_count),
		listen(std::make_shared<listener>(
			io_context, boost::asio::ip::tcp::endpoint{boost::asio::ip::address_v4::any(), port}))
{   
}

void server::server::serve()
{
	listen->run();

	std::vector<std::thread> threads;

	threads.reserve(thread_count - 1);

	for (int i = 0; i < thread_count - 1; i++)
	{
		threads.emplace_back([this]() { io_context.run(); });
	}

	io_context.run();
}

boost::asio::ip::port_type server::server::port()
{
	return listen->port();
}
