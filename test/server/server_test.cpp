#include <gtest/gtest.h>
#include <curl/curl.h>

#include <memory>
#include <thread>

#include "server/server.h"

size_t writer(void *ptr, size_t size, size_t nmemb, std::string* data)
{
	data->append((char*) ptr, size * nmemb);

	return size * nmemb;
}

class server_test: public ::testing::Test
{ 
protected:
	std::thread thread;

	boost::asio::ip::port_type port;

	void SetUp()
	{
		auto server = std::make_shared<server::server>(0, 2);
		port = server->port();

		thread = std::thread([server]()
			{
				server->serve();
			});
	}

	void TearDown()
	{
		thread.detach();
	}
};

TEST_F(server_test, get_request)
{
	auto curl = curl_easy_init();
	long http_code = 0;
	std::string response;

	struct curl_slist *headers = NULL;

	headers = curl_slist_append(headers, "Connection: close");
 
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	curl_easy_setopt(curl, CURLOPT_URL, "localhost");
	curl_easy_setopt(curl, CURLOPT_PORT, port);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writer);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	auto status = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);

	EXPECT_EQ(status, CURLE_OK);
	EXPECT_EQ(http_code, 200);
	EXPECT_EQ(response, "{ \"message\": \"Hello, world!\" }");	
}

TEST_F(server_test, post_request)
{
	auto curl = curl_easy_init();
	long http_code = 0;
	std::string response;

	struct curl_slist *headers = NULL;

	headers = curl_slist_append(headers, "Connection: close");

	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	curl_easy_setopt(curl, CURLOPT_URL, "localhost");
	curl_easy_setopt(curl, CURLOPT_PORT, port);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{ \"field\": \"value\" }");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writer);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	auto status = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);

	EXPECT_EQ(status, CURLE_OK);
	EXPECT_EQ(http_code, 200);
	EXPECT_EQ(response, "{ \"message\": \"Hello, world!\" }");   
}

TEST_F(server_test, head_request)
{
	auto curl = curl_easy_init();
	long http_code = 0;
	std::string response;

	struct curl_slist *headers = NULL;

	headers = curl_slist_append(headers, "Connection: close");
 
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	curl_easy_setopt(curl, CURLOPT_URL, "localhost");
	curl_easy_setopt(curl, CURLOPT_PORT, port);
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writer);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	auto status = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);

	EXPECT_EQ(status, CURLE_OK);
	EXPECT_EQ(http_code, 200);
	EXPECT_EQ(response, "");   
}

TEST_F(server_test, put_request)
{
	auto curl = curl_easy_init();
	long http_code = 0;
	std::string response;

	struct curl_slist *headers = NULL;

	headers = curl_slist_append(headers, "Connection: close");
 
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	curl_easy_setopt(curl, CURLOPT_URL, "localhost");
	curl_easy_setopt(curl, CURLOPT_PORT, port);
	curl_easy_setopt(curl, CURLOPT_PUT, 1);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writer);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	auto status = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);

	EXPECT_EQ(status, CURLE_OK);
	EXPECT_EQ(http_code, 400);
	EXPECT_EQ(response, "Unknown HTTP-method");   
}

TEST_F(server_test, two_get_requests)
{
	auto curl = curl_easy_init();
	long http_code = 0;
	std::string response;
 
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	curl_easy_setopt(curl, CURLOPT_URL, "localhost");
	curl_easy_setopt(curl, CURLOPT_PORT, port);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writer);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	EXPECT_EQ(http_code, 200);
	EXPECT_EQ(response, "{ \"message\": \"Hello, world!\" }");  
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
	EXPECT_EQ(response, "{ \"message\": \"Hello, world!\" }");   
}
