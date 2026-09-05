#include <chrono>
#include <thread>

#include "listening.h"

namespace
{
	// Ten seconds of them, which is a great deal longer than binding and joining take even under
	// valgrind, and is still an end.
	constexpr int attempts = 1000;

	constexpr std::chrono::milliseconds interval(10);
}

void server::wait_until_listening(boost::asio::ip::port_type port)
{
	boost::asio::io_context io_context;
	boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address("127.0.0.1"), port);

	for (int attempt = 0; attempt < attempts; attempt++)
	{
		boost::asio::ip::tcp::socket socket(io_context);
		boost::system::error_code error;

		socket.connect(endpoint, error);

		if (!error)
		{
			// The connection is closed rather than used: it was made to learn that it could be,
			// and the session the server made for it ends with it.
			socket.close(error);

			return;
		}

		std::this_thread::sleep_for(interval);
	}
}
