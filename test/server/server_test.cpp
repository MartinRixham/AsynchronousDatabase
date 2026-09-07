#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <curl/curl.h>
#include <boost/json.hpp>

#include "record/record.h"
#include "server/server.h"
#include "listening.h"

size_t writer(void *ptr, size_t size, size_t nmemb, std::string *stream)
{
	std::string temp(static_cast<const char *>(ptr), size * nmemb);
	stream->append(temp);

	return size * nmemb;
}

struct result
{
	CURLcode status;

	long code = 0;

	std::string body;

	std::string content_type;

	long long content_length = 0;
};

class server_test : public ::testing::Test
{
protected:
	std::shared_ptr<server::server> database_server;

	std::thread thread;

	boost::asio::ip::port_type port;

	void SetUp()
	{
		std::filesystem::remove_all("/tmp/asyncdb/");
		database_server = std::make_shared<server::server>(0, 2, "/tmp/asyncdb");
		port = database_server->port();

		thread = std::thread([server = database_server]() { server->serve(); });

		// The port is opened by serve() and not by the constructor, so it is waited for rather
		// than assumed.
		server::wait_until_listening(port);
	}

	// A detached thread would serve for ever, and the server it holds, its threads and its
	// database with it, so serving is stopped and waited for instead.
	void TearDown()
	{
		database_server->close();
		thread.join();
		database_server = nullptr;
	}

	result request(const std::string &method, const std::string &path, const std::string &body)
	{
		auto curl = curl_easy_init();
		result result;

		struct curl_slist *headers = NULL;

		headers = curl_slist_append(headers, "Connection: close");

		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(curl, CURLOPT_URL, ("localhost" + path).c_str());
		curl_easy_setopt(curl, CURLOPT_PORT, port);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writer);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);

		if (method == "HEAD")
		{
			curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
		}
		else if (method != "GET")
		{
			curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
		}

		result.status = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.code);

		char *content_type = NULL;
		curl_off_t content_length = 0;

		curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
		curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);

		result.content_type = content_type == NULL ? "" : content_type;
		result.content_length = content_length;

		curl_easy_cleanup(curl);
		curl_slist_free_all(headers);

		return result;
	}

	result get(const std::string &path)
	{
		return request("GET", path, "");
	}

	std::string error_code(const result &result)
	{
		return std::string(boost::json::parse(result.body).as_object().at("error").as_object().at("code").as_string());
	}
};

TEST_F(server_test, get_request)
{
	result response = get("/table");

	EXPECT_EQ(response.status, CURLE_OK);
	EXPECT_EQ(response.code, 200);
	EXPECT_EQ(response.body, "{\"tables\":[]}");
}

TEST_F(server_test, health_request)
{
	result response = get("/health");

	EXPECT_EQ(response.code, 200);
	EXPECT_EQ(response.body, "{\"status\":\"ok\",\"write_stalled\":false}");
}

TEST_F(server_test, put_request)
{
	result response = request("PUT", "/table/account", "{\"dependencies\":[]}");

	EXPECT_EQ(response.status, CURLE_OK);
	EXPECT_EQ(response.code, 201);
	EXPECT_EQ(response.body, "{\"name\":\"account\",\"dependencies\":[]}");

	EXPECT_EQ(get("/table").body, "{\"tables\":[{\"name\":\"account\",\"dependencies\":[]}]}");
}

TEST_F(server_test, put_request_with_an_invalid_name)
{
	result response = request("PUT", "/table/An%2FAccount", "{}");

	EXPECT_EQ(response.status, CURLE_OK);
	EXPECT_EQ(response.code, 400);
	EXPECT_EQ(error_code(response), "invalid_table_name");
}

TEST_F(server_test, put_request_with_a_body_that_is_not_json)
{
	result response = request("PUT", "/table/account", "not json");

	EXPECT_EQ(response.status, CURLE_OK);
	EXPECT_EQ(response.code, 400);
	EXPECT_EQ(error_code(response), "invalid_body");
}

TEST_F(server_test, write_then_read_a_record)
{
	request("PUT", "/table/account", "{}");

	result written = request("PUT", "/table/account/key/4821", "Eleanor Whitmore");

	EXPECT_EQ(written.code, 204);
	EXPECT_EQ(written.body, "");

	result response = get("/table/account/key/4821");

	EXPECT_EQ(response.code, 200);
	EXPECT_EQ(response.content_type, "text/plain; charset=utf-8");
	EXPECT_EQ(response.body, "Eleanor Whitmore");
}

// The documented limit is a value of 16 MiB and Beast's own default is a request body of one, so
// without a body limit of its own the parser ends the read in an error and the session closes the
// connection with no response at all — a documented limit the server never reaches, and a
// forwarded copy of a large value that dies on the wire between two nodes.
TEST_F(server_test, write_then_read_the_largest_value)
{
	request("PUT", "/table/account", "{}");

	result written = request("PUT", "/table/account/key/4821", std::string(record::max_value_size, 'v'));

	EXPECT_EQ(written.status, CURLE_OK);
	EXPECT_EQ(written.code, 204);

	result response = get("/table/account/key/4821");

	EXPECT_EQ(response.code, 200);
	EXPECT_EQ(response.body.size(), record::max_value_size);
}

// One byte over is the documented error rather than a closed connection, which is what the body
// limit being one byte above the value limit is for: the parser takes it and the router refuses
// it, instead of the parser refusing to read it and the session having nothing to answer with.
TEST_F(server_test, write_a_value_that_is_too_large)
{
	request("PUT", "/table/account", "{}");

	result response = request(
		"PUT", "/table/account/key/4821", std::string(record::max_value_size + 1, 'v'));

	EXPECT_EQ(response.status, CURLE_OK);
	EXPECT_EQ(response.code, 413);
	EXPECT_EQ(error_code(response), "value_too_large");
}

TEST_F(server_test, a_key_travels_percent_encoded)
{
	request("PUT", "/table/account", "{}");
	request("PUT", "/table/account/key/user%2F4821%3Fa%3Db", "a value");

	EXPECT_EQ(get("/table/account/key/user%2F4821%3Fa%3Db").body, "a value");
}

// The other documented limit that travels through Beast's own defaults. A key is 4 KiB and it
// travels percent encoded in the request line, so a key of bytes that all have to be encoded is
// three times that before the target is even decoded — and Beast reads 8 KiB of header by
// default, which is a documented key the server never sees.
TEST_F(server_test, write_then_read_the_largest_key)
{
	request("PUT", "/table/account", "{}");

	// Two bytes of UTF-8 each, and neither of them a character a path may carry unencoded, so
	// every byte of the key is three characters of the target.
	std::string key;

	for (size_t i = 0; i < record::max_key_size / 2; i++)
	{
		key += "%C3%A9";
	}

	result written = request("PUT", "/table/account/key/" + key, "a value");

	EXPECT_EQ(written.status, CURLE_OK);
	EXPECT_EQ(written.code, 204);

	EXPECT_EQ(get("/table/account/key/" + key).body, "a value");
}

TEST_F(server_test, read_a_record_that_is_not_there)
{
	request("PUT", "/table/account", "{}");

	result response = get("/table/account/key/4821");

	EXPECT_EQ(response.code, 404);
	EXPECT_EQ(response.body, "");
}

TEST_F(server_test, head_request)
{
	request("PUT", "/table/account", "{}");
	request("PUT", "/table/account/key/4821", "Eleanor Whitmore");

	result response = request("HEAD", "/table/account/key/4821", "");

	EXPECT_EQ(response.status, CURLE_OK);
	EXPECT_EQ(response.code, 200);
	EXPECT_EQ(response.body, "");

	// The cheap way to ask whether a key exists and how large it is.
	EXPECT_EQ(response.content_length, 16);
}

TEST_F(server_test, scan_request)
{
	request("PUT", "/table/account", "{}");
	request("PUT", "/table/account/key/user%3A4821", "Eleanor Whitmore");
	request("PUT", "/table/account/key/user%3A7203", "Marcus Hale");
	request("PUT", "/table/account/key/order%3A1", "an order");

	result response = get("/table/account/key?prefix=user%3A&limit=1");

	EXPECT_EQ(response.code, 200);

	boost::json::object body = boost::json::parse(response.body).as_object();
	boost::json::array records = body.at("records").as_array();

	EXPECT_EQ(records.size(), 1);
	EXPECT_EQ(records[0].as_object().at("key"), "user:4821");
	EXPECT_EQ(records[0].as_object().at("value"), "Eleanor Whitmore");

	std::string cursor = std::string(body.at("next").as_string());
	result next_result = get("/table/account/key?prefix=user%3A&limit=1&cursor=" + cursor);

	boost::json::object next_body = boost::json::parse(next_result.body).as_object();

	EXPECT_EQ(next_body.at("records").as_array()[0].as_object().at("key"), "user:7203");
	EXPECT_FALSE(next_body.contains("next"));
}

TEST_F(server_test, delete_a_record)
{
	request("PUT", "/table/account", "{}");
	request("PUT", "/table/account/key/4821", "Eleanor Whitmore");

	result response = request("DELETE", "/table/account/key/4821", "");

	EXPECT_EQ(response.code, 204);
	EXPECT_EQ(get("/table/account/key/4821").code, 404);
}

TEST_F(server_test, delete_a_range)
{
	request("PUT", "/table/account", "{}");
	request("PUT", "/table/account/key/user%3A4821", "Eleanor Whitmore");
	request("PUT", "/table/account/key/order%3A1", "an order");

	EXPECT_EQ(request("DELETE", "/table/account/key?prefix=user%3A", "").code, 204);
	EXPECT_EQ(request("DELETE", "/table/account/key", "").code, 400);

	boost::json::object body = boost::json::parse(get("/table/account/key").body).as_object();

	EXPECT_EQ(body.at("records").as_array().size(), 1);
}

TEST_F(server_test, delete_a_table)
{
	request("PUT", "/table/account", "{}");

	EXPECT_EQ(request("DELETE", "/table/account", "").code, 204);
	EXPECT_EQ(request("DELETE", "/table/account", "").code, 404);
	EXPECT_EQ(get("/table").body, "{\"tables\":[]}");
}

TEST_F(server_test, a_method_that_is_not_a_method_of_this_api)
{
	result response = request("POST", "/table/account", "{}");

	EXPECT_EQ(response.status, CURLE_OK);
	EXPECT_EQ(response.code, 405);
	EXPECT_EQ(error_code(response), "method_not_allowed");
}

TEST_F(server_test, two_get_requests)
{
	auto curl = curl_easy_init();
	long http_code = 0;
	std::string response;

	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	curl_easy_setopt(curl, CURLOPT_URL, "localhost/table");
	curl_easy_setopt(curl, CURLOPT_PORT, port);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writer);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	EXPECT_EQ(http_code, 200);
	EXPECT_EQ(response, "{\"tables\":[]}");
	response = "";

	struct curl_slist *headers = NULL;

	headers = curl_slist_append(headers, "Connection: close");

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	auto status = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);

	EXPECT_EQ(status, CURLE_OK);
	EXPECT_EQ(http_code, 200);
	EXPECT_EQ(response, "{\"tables\":[]}");
}

// The pool is a count of requests that can be waiting on another node rather than a count of
// cores: a forwarded request holds the thread it arrived on until the answer comes back, so a
// pool the size of a small instance's cores is a server two waiting requests can fill.
TEST(server_threads_test, size_the_pool_for_waiting_rather_than_for_cores)
{
	unsetenv("ASYNCDB_THREADS");

	EXPECT_GE(server::thread_pool_size(), 16);
	EXPECT_LE(server::thread_pool_size(), 128);
	EXPECT_GT(server::thread_pool_size(), static_cast<int>(std::thread::hardware_concurrency()));
}

TEST(server_threads_test, take_the_size_of_the_pool_from_the_environment)
{
	setenv("ASYNCDB_THREADS", "5", 1);

	EXPECT_EQ(server::thread_pool_size(), 5);

	// Something that is not a count of threads is nothing configured, and a server that serves on
	// no threads at all is what this is not allowed to answer.
	setenv("ASYNCDB_THREADS", "not a number", 1);

	EXPECT_GE(server::thread_pool_size(), 16);

	setenv("ASYNCDB_THREADS", "0", 1);

	EXPECT_GE(server::thread_pool_size(), 16);

	setenv("ASYNCDB_THREADS", "-4", 1);

	EXPECT_GE(server::thread_pool_size(), 16);

	unsetenv("ASYNCDB_THREADS");
}

// The store is one directory an instance opens and opens again, and where it is is configurable
// so that a test and a container do not have to agree about it.
TEST(server_threads_test, take_the_directory_of_the_store_from_the_environment)
{
	setenv("ASYNCDB_DATA", "/tmp/asyncdb_configured", 1);

	EXPECT_EQ(server::data_directory(), "/tmp/asyncdb_configured");

	// Nothing configured is the directory the image mounts a volume over.
	unsetenv("ASYNCDB_DATA");

	EXPECT_EQ(server::data_directory(), "/var/lib/asyncdb");

	setenv("ASYNCDB_DATA", "", 1);

	EXPECT_EQ(server::data_directory(), "/var/lib/asyncdb");

	unsetenv("ASYNCDB_DATA");
}
