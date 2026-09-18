#include <chrono>
#include <memory>
#include <signal.h>

#include <boost/json/src.hpp>

#include "server/server.h"
#include "cluster/etcd_cluster.h"
#include "cluster/http_forwarder.h"

std::shared_ptr<server::server> database_server;

std::chrono::seconds drain_seconds;

void handle_signal(int)
{
	// Closing the acceptor is what ends serving, and main leaves on its own once it has. Exiting
	// here instead would destroy the io_context under the threads still running it.
	database_server->drain(drain_seconds);
}

int main(void)
{
	drain_seconds = server::drain_interval();

	int thread_pool_size = server::thread_pool_size();
	std::string data_directory = server::data_directory();
	cluster::config configuration = cluster::from_environment();
	http::beast_client client =
		http::beast_client(configuration.connect_timeout_seconds, configuration.unacknowledged_timeout_seconds);
	cluster::http_forwarder forwarder = cluster::http_forwarder(client);
	cluster::etcd_cluster cluster = cluster::etcd_cluster(configuration, client);

	database_server = std::make_shared<server::server>(8080, thread_pool_size, cluster, forwarder, data_directory);

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	database_server->serve();

	// Serving has ended, so the server is destroyed here rather than by whatever is left of the
	// process at exit.
	database_server = nullptr;

	return EXIT_SUCCESS;
}
