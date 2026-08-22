#include <string>

#include <gtest/gtest.h>

#include "record/record.h"

TEST(record_test, every_string_is_a_key)
{
	record::record record = record::parse_key("user:4821");

	EXPECT_TRUE(record.is_valid);
	EXPECT_EQ(record.key, "user:4821");
}

TEST(record_test, a_key_may_contain_a_zero_byte)
{
	std::string key("4821\0" "2019", 9);

	EXPECT_TRUE(record::parse_key(key).is_valid);
}

TEST(record_test, the_empty_string_is_a_value)
{
	record::record record = record::parse_record("a key", "");

	EXPECT_TRUE(record.is_valid);
	EXPECT_EQ(record.value, "");
}

TEST(record_test, fail_to_read_a_key_that_is_not_utf8)
{
	record::record record = record::parse_key("\xc3\x28");

	EXPECT_FALSE(record.is_valid);
	EXPECT_EQ(record.code, "invalid_key_encoding");
}

TEST(record_test, fail_to_read_a_key_that_is_too_large)
{
	record::record record = record::parse_key(std::string(record::max_key_size + 1, 'k'));

	EXPECT_FALSE(record.is_valid);
	EXPECT_EQ(record.code, "key_too_large");
}

TEST(record_test, a_key_of_exactly_the_limit_is_a_key)
{
	EXPECT_TRUE(record::parse_key(std::string(record::max_key_size, 'k')).is_valid);
}

TEST(record_test, fail_to_read_a_value_that_is_too_large)
{
	record::record record = record::parse_record("a key", std::string(record::max_value_size + 1, 'v'));

	EXPECT_FALSE(record.is_valid);
	EXPECT_EQ(record.code, "value_too_large");
}

TEST(record_test, a_value_is_not_read_as_utf8)
{
	// The service keeps the bytes, not the structure, and does not look at a value at all.
	EXPECT_TRUE(record::parse_record("a key", "\xff\xfe").is_valid);
}

TEST(record_test, valid_utf8)
{
	EXPECT_TRUE(record::is_valid_utf8(""));
	EXPECT_TRUE(record::is_valid_utf8("plain ascii"));
	EXPECT_TRUE(record::is_valid_utf8("\xc3\xa9"));
	EXPECT_TRUE(record::is_valid_utf8("\xe2\x82\xac"));
	EXPECT_TRUE(record::is_valid_utf8("\xf0\x9f\x92\xa9"));
}

TEST(record_test, invalid_utf8)
{
	EXPECT_FALSE(record::is_valid_utf8("\x80"));
	EXPECT_FALSE(record::is_valid_utf8("\xc3"));
	EXPECT_FALSE(record::is_valid_utf8("\xe2\x82"));
	EXPECT_FALSE(record::is_valid_utf8("\xf8\x88\x80\x80\x80"));

	// An overlong encoding is a second spelling of a code point, and a second spelling of a key.
	EXPECT_FALSE(record::is_valid_utf8("\xc0\xaf"));

	// A surrogate is not a code point.
	EXPECT_FALSE(record::is_valid_utf8("\xed\xa0\x80"));
}
