#ifndef SERVER_LISTENING_H
#define SERVER_LISTENING_H

#include <boost/asio.hpp>

namespace server
{
	// Waits for a server started on another thread to open its port.
	//
	// A server binds in its constructor, which settles the port a test asks it for, but it listens
	// in serve() — after it has filled its store and joined — so a test that started serve() on a
	// thread of its own is racing it. Connecting until the connection is taken is how it stops
	// racing; it gives up rather than hanging the suite on a server that never listens.
	void wait_until_listening(boost::asio::ip::port_type port);
}

#endif
