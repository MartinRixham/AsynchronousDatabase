#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <curl/curl.h>
#include <boost/json.hpp>

#include "server/server.h"

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
		database_server = std::make_shared<server::server>(0, 2);
		port = database_server->port();

		thread = std::thread([server = database_server]() { server->serve(); });
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

TEST_F(server_test, a_key_travels_percent_encoded)
{
	request("PUT", "/table/account", "{}");
	request("PUT", "/table/account/key/user%2F4821%3Fa%3Db", "a value");

	EXPECT_EQ(get("/table/account/key/user%2F4821%3Fa%3Db").body, "a value");
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
