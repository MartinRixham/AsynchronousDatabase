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

TEST(Server, get_request)
{
    auto srv = std::make_shared<server::server>("127.0.0.1", 0, 2);
    int port = srv->port();

    auto thread = std::thread([srv]()
        {
            srv->serve();
        });

    auto curl = curl_easy_init();
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

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    thread.detach();

    EXPECT_EQ(status, CURLE_OK);
    EXPECT_EQ(response, "{ \"message\": \"Hello, world!\" }");   
}
