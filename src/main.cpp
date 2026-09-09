#include <memory>
#include <signal.h>

#include <curl/curl.h>
#include <boost/json/src.hpp>

#include "server/server.h"
#include "cluster/etcd_cluster.h"

std::shared_ptr<server::server> database_server;

void handle_signal(int)
{
	// Closing the acceptor is what ends serving, and main leaves on its own once it has. Exiting
	// here instead would destroy the io_context under the threads still running it.
	database_server->close();
}

int main(void)
{
	// libcurl is initialised once here rather than by the first handle to be created, because a
	// node talks to etcd and to its neighbours from several threads at once.
	curl_global_init(CURL_GLOBAL_DEFAULT);

	int thread_pool_size = server::thread_pool_size();
	std::string data_directory = server::data_directory();
	cluster::config configuration = cluster::from_environment();
	http::curl_client client = http::curl_client(configuration.timeout_seconds, configuration.connect_timeout_seconds);
	cluster::etcd_cluster cluster = cluster::etcd_cluster(configuration, client);

	database_server = std::make_shared<server::server>(8080, thread_pool_size, cluster, data_directory);

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	database_server->serve();

	// Serving has ended, so the server is destroyed here rather than by whatever is left of the
	// process at exit.
	database_server = nullptr;

	return EXIT_SUCCESS;
}
