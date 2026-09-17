#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "http/beast_client.h"
#include "server/listening.h"
#include "cluster/etcd_cluster.h"
#include "cluster/forwarder.h"
#include "server/server.h"
#include "directory.h"

namespace
{
	// An answer that says the connection is being kept and then closes it anyway, which is what a
	// connection nobody has used for long enough is. It answers once for each time it is asked.
	void answer_and_close(boost::asio::ip::tcp::acceptor *acceptor, int times)
	{
		for (int asked = 0; asked < times; asked++)
		{
			boost::asio::ip::tcp::socket socket = acceptor->accept();
			boost::asio::streambuf request;
			boost::system::error_code error;

			boost::asio::read_until(socket, request, "\r\n\r\n", error);

			boost::asio::write(
				socket,
				boost::asio::buffer(std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")),
				error);

			socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
			socket.close(error);
		}
	}

	// An answer that sends one piece and then holds the connection open without ending, which is
	// what a watch is. It returns once the client has gone.
	void answer_without_ending(boost::asio::ip::tcp::acceptor *acceptor, const std::string &piece)
	{
		boost::asio::ip::tcp::socket socket = acceptor->accept();
		boost::asio::streambuf request;
		boost::system::error_code error;

		boost::asio::read_until(socket, request, "\r\n\r\n", error);

		std::string chunk = (std::ostringstream() << std::hex << piece.size()).str() + "\r\n" + piece + "\r\n";

		boost::asio::write(
			socket,
			boost::asio::buffer(
				"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n" + chunk),
			error);

		char ignored[256];

		while (!error)
		{
			socket.read_some(boost::asio::buffer(ignored), error);
		}
	}
}

// A client with a real server to talk to, because what is worth testing here is what the client
// does between one request and the next, and a fake would answer for neither.
class beast_client_test : public ::testing::Test
{
protected:
	http::beast_client client { http::beast_client(2, 5) };

	cluster::forwarder forwarder = cluster::forwarder(client);

	cluster::etcd_cluster cluster = cluster::etcd_cluster(cluster::config(), client, forwarder);

	std::shared_ptr<server::server> database_server;

	std::thread thread;

	bool serving = false;

	void SetUp()
	{
		std::filesystem::remove_all(test_directory("asyncdb"));

		database_server = std::make_shared<server::server>(0, 2, cluster, test_directory("asyncdb"));
		serving = true;
		thread = std::thread([server = database_server]() { server->serve(); });

		// The port is opened by serve() and not by the constructor, so it is waited for rather
		// than assumed. A request that beats the serving thread to it is refused at once, which
		// is a client that answers no answer rather than a server that is not there.
		server::wait_until_listening(database_server->port());
	}

	void TearDown()
	{
		stop();

		database_server = nullptr;
	}

	void stop()
	{
		if (serving)
		{
			serving = false;

			database_server->close();
			thread.join();
		}
	}

	http::request get(const std::string &path)
	{
		return { "GET", "http://localhost:" + std::to_string(database_server->port()) + path, "", {} };
	}
};

TEST_F(beast_client_test, answer_a_request)
{
	http::response response = client.send(get("/health"), 30);

	EXPECT_TRUE(response.is_valid);
	EXPECT_EQ(response.status, 200);
	EXPECT_EQ(response.content_type, "application/json");
	EXPECT_NE(response.body.find("\"status\":\"ok\""), std::string::npos);
}

// The point of holding the connections for the life of a thread: a node talks to the same
// neighbours over and over, and the second request is not another three way handshake.
TEST_F(beast_client_test, keep_the_connection_between_requests)
{
	EXPECT_FALSE(client.send(get("/health"), 30).reused);
	EXPECT_TRUE(client.send(get("/health"), 30).reused);
	EXPECT_TRUE(client.send(get("/health"), 30).reused);
}

// A connection out of the pool may have been closed at the other end while nothing was going on
// it, and what says so is the request that fails on it. That is a request to make again on a
// connection of its own rather than a node that did not answer.
TEST_F(beast_client_test, make_a_request_again_on_a_connection_the_node_closed)
{
	boost::asio::io_context context;
	boost::asio::ip::tcp::acceptor acceptor(
		context,
		boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
	std::thread answering([&acceptor]() { answer_and_close(&acceptor, 2); });

	http::request asked = get("/health");

	asked.url = "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/health";

	EXPECT_EQ(client.send(asked, 30).body, "ok");

	http::response again = client.send(asked, 30);

	answering.join();

	EXPECT_TRUE(again.is_valid);
	EXPECT_EQ(again.body, "ok");
	EXPECT_FALSE(again.reused);
}

// A connection that is used again carries the request it was given and nothing of the one before
// it: the "no body" of a HEAD is the answer to that HEAD alone.
TEST_F(beast_client_test, forget_the_request_before)
{
	http::request head = get("/health");

	head.method = "HEAD";

	EXPECT_TRUE(client.send(head, 30).body.empty());

	http::response response = client.send(get("/health"), 30);

	EXPECT_EQ(response.status, 200);
	EXPECT_FALSE(response.body.empty());
}

// What makes a HEAD worth sending as a HEAD: the node answers how large the value is and sends
// none of it, so asking whether a record exists costs its headers rather than its megabytes.
TEST_F(beast_client_test, answer_the_length_of_a_body_a_head_left_out)
{
	http::request head = get("/health");

	head.method = "HEAD";

	http::response response = client.send(head, 30);

	EXPECT_EQ(response.status, 200);
	EXPECT_TRUE(response.body.empty());
	EXPECT_GT(response.content_length, 0);
}

TEST_F(beast_client_test, answer_that_there_was_no_answer)
{
	http::request request = get("/health");

	// A port nothing listens on, which is a node that is gone rather than a node that refused.
	request.url = "http://localhost:1/health";

	http::response response = client.send(request, 30);

	EXPECT_FALSE(response.is_valid);
	EXPECT_FALSE(response.message.empty());
}

// The other side of keeping connections: a connection nobody is using still belongs to a server
// that is trying to stop, and shutting down waits for connections rather than for their timeouts.
TEST_F(beast_client_test, a_connection_that_is_kept_does_not_hold_the_server_open)
{
	EXPECT_TRUE(client.send(get("/health"), 30).is_valid);

	std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

	stop();

	std::chrono::seconds taken =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);

	EXPECT_LT(taken.count(), 5);
}

// The fan out that keeps a node's threads: every request is in flight at once, and each answer
// belongs to the request it was asked for whatever order the transfers finished in.
TEST_F(beast_client_test, answer_every_request_of_a_fan_out)
{
	std::vector<http::response> responses =
		client.send_all({ get("/health"), get("/table/nothing"), get("/table") }, 30);

	ASSERT_EQ(responses.size(), 3u);

	EXPECT_EQ(responses[0].status, 200);
	EXPECT_NE(responses[0].body.find("\"status\":\"ok\""), std::string::npos);

	EXPECT_EQ(responses[1].status, 404);
	EXPECT_NE(responses[1].body.find("table_not_found"), std::string::npos);

	EXPECT_EQ(responses[2].status, 200);
	EXPECT_NE(responses[2].body.find("\"tables\""), std::string::npos);
}

// The body of a request is not copied into the message it is sent as, so a fan out has to hold
// its requests still until it has run. Two writes carrying different bodies are two records.
TEST_F(beast_client_test, carry_the_body_of_every_request_of_a_fan_out)
{
	http::request table = get("/table/account");

	table.method = "PUT";
	table.body = "{}";

	ASSERT_EQ(client.send(table, 30).status, 201);

	http::request first = get("/table/account/key/1");
	http::request second = get("/table/account/key/2");

	first.method = "PUT";
	first.body = "the first value";
	second.method = "PUT";
	second.body = "the second value";

	std::vector<http::response> written = client.send_all({ first, second }, 30);

	ASSERT_EQ(written.size(), 2u);
	EXPECT_EQ(written[0].status, 204);
	EXPECT_EQ(written[1].status, 204);

	EXPECT_EQ(client.send(get("/table/account/key/1"), 30).body, "the first value");
	EXPECT_EQ(client.send(get("/table/account/key/2"), 30).body, "the second value");
}

// A node of a fan out that is not there is answered against on its own, and the nodes that did
// answer answer all the same — which is what lets a write say which copy refused it.
TEST_F(beast_client_test, answer_that_a_node_of_a_fan_out_did_not_answer)
{
	http::request gone = get("/health");

	// A port nothing listens on, which is a node that is gone rather than a node that refused.
	gone.url = "http://localhost:1/health";

	std::vector<http::response> responses = client.send_all({ gone, get("/health") }, 30);

	ASSERT_EQ(responses.size(), 2u);

	EXPECT_FALSE(responses[0].is_valid);
	EXPECT_FALSE(responses[0].message.empty());

	EXPECT_TRUE(responses[1].is_valid);
	EXPECT_EQ(responses[1].status, 200);
}

// A fan out is a connection for each request in it, and every one of them is kept for the reason
// one request's is: a node writes to the same copies over and over.
TEST_F(beast_client_test, keep_the_connections_of_a_fan_out_between_them)
{
	std::vector<http::request> requests { get("/health"), get("/table") };

	std::vector<http::response> first = client.send_all(requests, 30);

	ASSERT_EQ(first.size(), 2u);
	EXPECT_FALSE(first[0].reused);
	EXPECT_FALSE(first[1].reused);

	std::vector<http::response> second = client.send_all(requests, 30);

	ASSERT_EQ(second.size(), 2u);
	EXPECT_TRUE(second[0].reused);
	EXPECT_TRUE(second[1].reused);
}

// One request has nothing to overlap with, and takes the connection this thread already holds
// rather than opening one of its own.
TEST_F(beast_client_test, answer_a_fan_out_of_one_on_a_connection_of_the_thread)
{
	EXPECT_EQ(client.send(get("/health"), 30).status, 200);

	std::vector<http::response> responses = client.send_all({ get("/health") }, 30);

	ASSERT_EQ(responses.size(), 1u);
	EXPECT_EQ(responses[0].status, 200);
	EXPECT_TRUE(responses[0].reused);
}

TEST_F(beast_client_test, answer_a_fan_out_of_nothing)
{
	EXPECT_TRUE(client.send_all({}, 30).empty());
}

// A thread holds a connection to every node it forwards to, and a cache smaller than the
// neighbour count of a cluster is the destination a thread used longest ago evicted, and a
// handshake again on every forward to it. Every address in 127.0.0.0/8 is this machine, so eight
// of them are eight connections to one server.
TEST_F(beast_client_test, keeps_a_connection_to_more_nodes_than_a_cluster_has)
{
	std::vector<http::request> nodes;

	for (size_t i = 1; i <= 8; i++)
	{
		http::request asked = get("/health");

		asked.url =
			"http://127.0.0." + std::to_string(i) + ":" + std::to_string(database_server->port()) + "/health";

		nodes.push_back(asked);
	}

	for (const auto &node : nodes)
	{
		EXPECT_FALSE(client.send(node, 30).reused) << node.url;
	}

	// Every one of them again, and not one was dropped to make room for the others.
	for (const auto &target : nodes)
	{
		EXPECT_TRUE(client.send(target, 30).reused) << target.url;
	}
}

// A piece of a stream is handed on while the answer is still going, and stopping is what ends it.
TEST_F(beast_client_test, hand_on_a_stream_as_it_arrives_until_it_is_stopped)
{
	boost::asio::io_context context;
	boost::asio::ip::tcp::acceptor acceptor(
		context,
		boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
	std::thread streaming([&acceptor]() { answer_without_ending(&acceptor, "{\"result\":{}}\n"); });
	std::stop_source stop;
	std::string received;

	http::request watch = get("/");

	watch.method = "POST";
	watch.url = "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/v3/watch";
	watch.body = "{}";

	http::response response = client.stream(
		watch,
		[&received, &stop](std::string_view piece)
		{
			received.append(piece);
			stop.request_stop();

			return true;
		},
		stop.get_token());

	streaming.join();

	EXPECT_EQ(received, "{\"result\":{}}\n");
	EXPECT_FALSE(response.is_valid);
}

TEST_F(beast_client_test, end_a_stream_the_receiver_is_done_with)
{
	boost::asio::io_context context;
	boost::asio::ip::tcp::acceptor acceptor(
		context,
		boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
	std::thread streaming([&acceptor]() { answer_without_ending(&acceptor, "cancelled\n"); });
	std::stop_source stop;

	http::request watch = get("/");

	watch.url = "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/v3/watch";

	http::response response =
		client.stream(watch, [](std::string_view) { return false; }, stop.get_token());

	streaming.join();

	EXPECT_FALSE(response.is_valid);
	EXPECT_FALSE(stop.stop_requested());
}
