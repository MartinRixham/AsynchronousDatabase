#include <string>

#include <gtest/gtest.h>

#include "url/url.h"

TEST(url_test, split_path_of_a_record)
{
	std::vector<std::string> path = url::split_path("/table/account/key/4821");

	EXPECT_EQ(path.size(), 4);
	EXPECT_EQ(path[0], "table");
	EXPECT_EQ(path[1], "account");
	EXPECT_EQ(path[2], "key");
	EXPECT_EQ(path[3], "4821");
}

TEST(url_test, split_path_without_the_query)
{
	std::vector<std::string> path = url::split_path("/table/account/key?prefix=user:&limit=10");

	EXPECT_EQ(path.size(), 3);
	EXPECT_EQ(path[2], "key");
}

TEST(url_test, trailing_slash_is_the_range_and_not_an_empty_key)
{
	std::vector<std::string> path = url::split_path("/table/account/key/");

	EXPECT_EQ(path.size(), 3);
}

TEST(url_test, decode_a_key_with_a_slash_in_it)
{
	std::vector<std::string> path = url::split_path("/table/account/key/user%2F4821");

	EXPECT_EQ(path.size(), 4);
	EXPECT_EQ(path[3], "user/4821");
}

TEST(url_test, decode_a_composite_key_separated_by_a_zero_byte)
{
	std::vector<std::string> path = url::split_path("/table/account/key/4821%002019");

	EXPECT_EQ(path.size(), 4);
	EXPECT_EQ(path[3], std::string("4821\0" "2019", 9));
}

TEST(url_test, read_a_query_parameter)
{
	std::string query = url::query_string("/table/account/key?prefix=user%3A&limit=10");

	EXPECT_EQ(query, "prefix=user%3A&limit=10");
	EXPECT_EQ(url::read_parameter(query, "prefix"), "user:");
	EXPECT_EQ(url::read_parameter(query, "limit"), "10");
}

TEST(url_test, read_a_parameter_that_is_not_there)
{
	EXPECT_EQ(url::read_parameter(url::query_string("/table"), "prefix"), "");
	EXPECT_EQ(url::read_parameter("prefix=user", "limit"), "");
}

TEST(url_test, a_parameter_is_not_read_from_the_name_of_another)
{
	EXPECT_EQ(url::read_parameter("prefixes=a&prefix=b", "prefix"), "b");
}
