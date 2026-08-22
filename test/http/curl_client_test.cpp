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
