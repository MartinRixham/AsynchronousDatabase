#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "http/http_client.h"
#include "server/server.h"

// A client with a real server to talk to, because what is worth testing here is what libcurl does
// between one request and the next, and a fake would answer for neither.
class curl_client_test : public ::testing::Test
{
protected:
	http::curl_client client { http::curl_client(10) };

	std::shared_ptr<server::server> database_server;

	std::thread thread;

	bool serving = false;

	void SetUp()
	{
		std::filesystem::remove_all("/tmp/asyncdb/");

		database_server = std::make_shared<server::server>(0, 2);
		serving = true;
		thread = std::thread([server = database_server]() { server->serve(); });
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

TEST_F(curl_client_test, answer_a_request)
{
	http::response response = client.send(get("/health"));

	EXPECT_TRUE(response.is_valid);
	EXPECT_EQ(response.status, 200);
	EXPECT_EQ(response.content_type, "application/json");
	EXPECT_NE(response.body.find("\"status\":\"ok\""), std::string::npos);
}

// The point of holding a handle for the life of a thread: a node talks to the same neighbours over
// and over, and the second request is not another three way handshake.
TEST_F(curl_client_test, keep_the_connection_between_requests)
{
	EXPECT_FALSE(client.send(get("/health")).reused);
	EXPECT_TRUE(client.send(get("/health")).reused);
	EXPECT_TRUE(client.send(get("/health")).reused);
}

// A handle that is used again is a handle that remembers what it was told last time, which is why
// it is reset: the "no body" of a HEAD would otherwise be the answer to every GET after it.
TEST_F(curl_client_test, forget_the_request_before)
{
	http::request head = get("/health");

	head.method = "HEAD";

	EXPECT_TRUE(client.send(head).body.empty());

	http::response response = client.send(get("/health"));

	EXPECT_EQ(response.status, 200);
	EXPECT_FALSE(response.body.empty());
}

TEST_F(curl_client_test, answer_that_there_was_no_answer)
{
	http::request request = get("/health");

	// A port nothing listens on, which is a node that is gone rather than a node that refused.
	request.url = "http://localhost:1/health";

	http::response response = client.send(request);

	EXPECT_FALSE(response.is_valid);
	EXPECT_FALSE(response.message.empty());
}

// The other side of keeping connections: a connection nobody is using still belongs to a server
// that is trying to stop, and shutting down waits for connections rather than for their timeouts.
TEST_F(curl_client_test, a_connection_that_is_kept_does_not_hold_the_server_open)
{
	EXPECT_TRUE(client.send(get("/health")).is_valid);

	std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

	stop();

	std::chrono::seconds taken =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);

	EXPECT_LT(taken.count(), 5);
}

// The fan out that keeps a node's threads: every request is in flight at once, and each answer
// belongs to the request it was asked for whatever order the transfers finished in.
TEST_F(curl_client_test, answer_every_request_of_a_fan_out)
{
	std::vector<http::response> responses =
		client.send_all({ get("/health"), get("/table/nothing"), get("/table") });

	ASSERT_EQ(responses.size(), 3u);

	EXPECT_EQ(responses[0].status, 200);
	EXPECT_NE(responses[0].body.find("\"status\":\"ok\""), std::string::npos);

	EXPECT_EQ(responses[1].status, 404);
	EXPECT_NE(responses[1].body.find("table_not_found"), std::string::npos);

	EXPECT_EQ(responses[2].status, 200);
	EXPECT_NE(responses[2].body.find("\"tables\""), std::string::npos);
}

// The body of a request is not copied into the handle it runs on, so a fan out has to hold its
// requests still until it has run. Two writes carrying different bodies are two records.
TEST_F(curl_client_test, carry_the_body_of_every_request_of_a_fan_out)
{
	http::request table = get("/table/account");

	table.method = "PUT";
	table.body = "{}";

	ASSERT_EQ(client.send(table).status, 201);

	http::request first = get("/table/account/key/1");
	http::request second = get("/table/account/key/2");

	first.method = "PUT";
	first.body = "the first value";
	second.method = "PUT";
	second.body = "the second value";

	std::vector<http::response> written = client.send_all({ first, second });

	ASSERT_EQ(written.size(), 2u);
	EXPECT_EQ(written[0].status, 204);
	EXPECT_EQ(written[1].status, 204);

	EXPECT_EQ(client.send(get("/table/account/key/1")).body, "the first value");
	EXPECT_EQ(client.send(get("/table/account/key/2")).body, "the second value");
}

// A node of a fan out that is not there is answered against on its own, and the nodes that did
// answer answer all the same — which is what lets a write say which copy refused it.
TEST_F(curl_client_test, answer_that_a_node_of_a_fan_out_did_not_answer)
{
	http::request gone = get("/health");

	// A port nothing listens on, which is a node that is gone rather than a node that refused.
	gone.url = "http://localhost:1/health";

	std::vector<http::response> responses = client.send_all({ gone, get("/health") });

	ASSERT_EQ(responses.size(), 2u);

	EXPECT_FALSE(responses[0].is_valid);
	EXPECT_FALSE(responses[0].message.empty());

	EXPECT_TRUE(responses[1].is_valid);
	EXPECT_EQ(responses[1].status, 200);
}

// The multi handle holds the connections of every transfer run in it, for the reason the single
// handle holds its own: a node writes to the same copies over and over.
TEST_F(curl_client_test, keep_the_connections_of_a_fan_out_between_them)
{
	std::vector<http::request> requests { get("/health"), get("/table") };

	std::vector<http::response> first = client.send_all(requests);

	ASSERT_EQ(first.size(), 2u);
	EXPECT_FALSE(first[0].reused);
	EXPECT_FALSE(first[1].reused);

	std::vector<http::response> second = client.send_all(requests);

	ASSERT_EQ(second.size(), 2u);
	EXPECT_TRUE(second[0].reused);
	EXPECT_TRUE(second[1].reused);
}

// One request has nothing to overlap with, so it runs on the handle that already holds this
// thread's connections rather than opening one of its own.
TEST_F(curl_client_test, answer_a_fan_out_of_one_on_the_handle_of_the_thread)
{
	EXPECT_EQ(client.send(get("/health")).status, 200);

	std::vector<http::response> responses = client.send_all({ get("/health") });

	ASSERT_EQ(responses.size(), 1u);
	EXPECT_EQ(responses[0].status, 200);
	EXPECT_TRUE(responses[0].reused);
}

TEST_F(curl_client_test, answer_a_fan_out_of_nothing)
{
	EXPECT_TRUE(client.send_all({}).empty());
}
