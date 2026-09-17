#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "bound_idle.h"

bool http::bound_idle(int socket, long seconds)
{
	int on = 1;
	int interval = static_cast<int>(seconds);

	return setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) == 0 &&
		setsockopt(socket, IPPROTO_TCP, TCP_KEEPIDLE, &interval, sizeof(interval)) == 0 &&
		setsockopt(socket, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) == 0;
}
