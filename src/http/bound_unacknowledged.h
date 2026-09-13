#ifndef HTTP_BOUND_UNACKNOWLEDGED_H
#define HTTP_BOUND_UNACKNOWLEDGED_H

namespace http
{
	// TCP_USER_TIMEOUT on a connected socket: what was sent on it and never acknowledged ends the
	// connection after this many seconds. False is a socket that refused the option.
	bool bound_unacknowledged(int socket, long seconds);
}

#endif
