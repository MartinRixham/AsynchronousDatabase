#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "http/bound_unacknowledged.h"

TEST(bound_unacknowledged_test, bound_how_long_a_connection_goes_unacknowledged)
{
	int socket = ::socket(AF_INET, SOCK_STREAM, 0);

	ASSERT_GE(socket, 0);

	EXPECT_TRUE(http::bound_unacknowledged(socket, 5));

	unsigned int milliseconds = 0;
	socklen_t length = sizeof(milliseconds);

	getsockopt(socket, IPPROTO_TCP, TCP_USER_TIMEOUT, &milliseconds, &length);

	EXPECT_EQ(milliseconds, 5000u);

	close(socket);
}

TEST(bound_unacknowledged_test, answer_that_a_socket_refused_it)
{
	int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);

	ASSERT_GE(socket, 0);

	EXPECT_FALSE(http::bound_unacknowledged(socket, 5));

	close(socket);
}
