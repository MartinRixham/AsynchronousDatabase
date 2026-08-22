#include <memory>
#include <signal.h>

#include <curl/curl.h>
#include <boost/json/src.hpp>

#include "server/server.h"

std::shared_ptr<server::server> database_server;

void handle_signal(int)
{
	// Closing the acceptor is what ends serving, and main leaves on its own once it has. Exiting
	// from here instead would destroy the io_context under the threads still running it, and the
	// process would hang on the way out rather than stop.
	database_server->close();
}

int main(void)
{
	// libcurl is initialised once here rather than by the first handle to be created, because a
	// node talks to etcd and to its neighbours from several threads at once.
	curl_global_init(CURL_GLOBAL_DEFAULT);

	database_server = std::make_shared<server::server>(8080);

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	database_server->serve();

	// Serving has ended, so the server is destroyed here rather than by whatever is left of the
	// process at exit.
	database_server = nullptr;

	return EXIT_SUCCESS;
}
