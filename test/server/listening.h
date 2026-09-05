#ifndef SERVER_LISTENING_H
#define SERVER_LISTENING_H

#include <boost/asio.hpp>

namespace server
{
	// Waits for a server started on another thread to open its port.
	//
	// A server binds in its constructor, which is what settles the port a test asks it for, but it
	// listens in serve() — after it has filled its store and joined — so that a node which is not
	// ready refuses connections instead of taking them and answering nothing. A test that started
	// serve() on a thread of its own is therefore racing it, and connecting until the connection
	// is taken is how it stops racing: what is refused is a server that has not got there yet.
	//
	// It gives up rather than waiting for ever, because a server that never listens is a test to
	// fail on its assertions and not one to hang the suite.
	void wait_until_listening(boost::asio::ip::port_type port);
}

#endif
