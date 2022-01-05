#include <gtest/gtest.h>
#include <curl/curl.h>

#include <memory>

#include "server/server.h"

TEST(Server, DISABLED_get_request)
{
    auto srv = std::make_shared<server::server>("127.0.0.1", 0);
    auto port = srv->port();

    std::thread([srv]()
        {
            srv->serve();
        });

    EXPECT_EQ(1, 2);

    auto curl = curl_easy_init();
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, "localhost:" + std::to_string(port));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    EXPECT_EQ(response, "Hello, world!");
}