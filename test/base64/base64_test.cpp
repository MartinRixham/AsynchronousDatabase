#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "base64/base64.h"

TEST(base64_test, encode_nothing)
{
	EXPECT_EQ(base64::encode(""), "");
}

TEST(base64_test, encode_text)
{
	EXPECT_EQ(base64::encode("asyncdb"), "YXN5bmNkYg==");
}

TEST(base64_test, pad_to_a_multiple_of_four)
{
	EXPECT_EQ(base64::encode("a"), "YQ==");
	EXPECT_EQ(base64::encode("ab"), "YWI=");
	EXPECT_EQ(base64::encode("abc"), "YWJj");
}

TEST(base64_test, encode_a_zero_byte)
{
	EXPECT_EQ(base64::encode(std::string("a\0b", 3)), "YQBi");
}

TEST(base64_test, decode_text)
{
	std::optional<std::string> decoded = base64::decode("YXN5bmNkYg==");

	ASSERT_TRUE(decoded);
	EXPECT_EQ(*decoded, "asyncdb");
}

TEST(base64_test, decode_a_zero_byte)
{
	std::optional<std::string> decoded = base64::decode("YQBi");

	ASSERT_TRUE(decoded);
	EXPECT_EQ(*decoded, std::string("a\0b", 3));
}

TEST(base64_test, decode_what_it_encoded)
{
	std::string text = "/asyncdb/node/http://asyncdb-1:8080";
	std::optional<std::string> decoded = base64::decode(base64::encode(text));

	ASSERT_TRUE(decoded);
	EXPECT_EQ(*decoded, text);
}

TEST(base64_test, fail_to_decode_text_that_is_not_base64)
{
	EXPECT_FALSE(base64::decode("not base 64").has_value());
}
