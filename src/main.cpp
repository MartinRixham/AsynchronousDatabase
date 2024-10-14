#include <memory>
#include <signal.h>

#include <boost/json/src.hpp>

#include "server/server.h"

std::shared_ptr<server::server> database_server;

void handle_signal(int signum)
{
	database_server->close();

	exit(signum);
}

int main(void)
{
	database_server = std::make_shared<server::server>(8080);

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	database_server->serve();

	return EXIT_SUCCESS;
}
