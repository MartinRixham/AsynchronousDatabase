#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "bound_unacknowledged.h"

bool http::bound_unacknowledged(int socket, long seconds)
{
	unsigned int milliseconds = static_cast<unsigned int>(seconds * 1000);

	return setsockopt(socket, IPPROTO_TCP, TCP_USER_TIMEOUT, &milliseconds, sizeof(milliseconds)) == 0;
}
