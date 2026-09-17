#pragma once

namespace http
{
	// TCP keepalive on a connected socket: a connection nothing is sent on is probed this often,
	// and one that answers no probe is ended. False is a socket that refused the option.
	bool bound_idle(int socket, long seconds);
}
