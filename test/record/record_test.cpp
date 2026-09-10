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

TEST(record_test, a_key_is_counted_in_the_bytes_of_its_encoding)
{
	// Two bytes each, so half as many characters as the limit is a key and one more is not.
	std::string key;

	for (size_t i = 0; i < record::max_key_size / 2; i++)
	{
		key += "\xc3\xa9";
	}

	EXPECT_TRUE(record::parse_key(key).is_valid);
	EXPECT_EQ(record::parse_key(key + "\xc3\xa9").code, "key_too_large");
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

TEST(record_test, a_key_of_one_part_is_the_bytes_it_was_given)
{
	EXPECT_EQ(record::compose_key("4821", ""), "4821");
}

TEST(record_test, a_key_of_two_parts_carries_a_separator_between_them)
{
	EXPECT_EQ(record::compose_key("4821", "2019"), std::string("4821\0" "2019", 9));
}

TEST(record_test, the_halves_of_a_key_come_back)
{
	std::string key = record::compose_key("4821", "2019");

	EXPECT_EQ(record::partition_key(key), "4821");
	EXPECT_EQ(record::sort_key(key), "2019");
}

TEST(record_test, a_key_of_one_part_sorts_under_nothing)
{
	EXPECT_EQ(record::partition_key("4821"), "4821");
	EXPECT_EQ(record::sort_key("4821"), "");
}

// A key written as one string with a zero byte in it and the same key written as two halves are
// one record, because it is the first zero byte that separates either way.
TEST(record_test, a_key_carrying_a_zero_byte_is_its_two_halves)
{
	std::string key("4821\0" "2019", 9);

	EXPECT_EQ(record::compose_key(key, ""), record::compose_key("4821", "2019"));
	EXPECT_EQ(record::partition_key(key), "4821");
	EXPECT_EQ(record::sort_key(key), "2019");
}

// The sort key of a key of two parts may carry one, because it is only the first that separates.
TEST(record_test, a_sort_key_may_carry_a_zero_byte)
{
	std::string key = record::compose_key("4821", std::string("2019\0" "1", 6));

	EXPECT_EQ(record::partition_key(key), "4821");
	EXPECT_EQ(record::sort_key(key), std::string("2019\0" "1", 6));
}

// The separator sorts below every other byte, which is what makes the store's order the order of
// the pair: every record of a partition key is together, in sort key order, and before any key the
// partition key is a prefix of.
TEST(record_test, keys_sort_as_the_pair_they_are)
{
	EXPECT_LT(record::compose_key("a", ""), record::compose_key("a", "b"));
	EXPECT_LT(record::compose_key("a", "b"), record::compose_key("a", "c"));
	EXPECT_LT(record::compose_key("a", "c"), record::compose_key("ab", ""));
}

TEST(record_test, a_key_of_two_parts_is_counted_in_the_bytes_of_both)
{
	std::string half(record::max_key_size / 2, 'k');

	EXPECT_TRUE(record::parse_key(record::compose_key(half, half.substr(1))).is_valid);
	EXPECT_EQ(record::parse_key(record::compose_key(half, half)).code, "key_too_large");
}
