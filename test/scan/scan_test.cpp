#include <expected>
#include <string>

#include <gtest/gtest.h>

#include "cluster/partition.h"
#include "scan/scan.h"

namespace
{
	// A scan names the partition it reads, so every range below is of one. Seven is nothing but a
	// partition that exists.
	const std::string partition = "partition=7";

	std::expected<scan::range, error::error_message> parsed(
		const std::string &query,
		const std::string &instance = "an instance")
	{
		return scan::parse_range(query.empty() ? partition : partition + "&" + query, instance);
	}

	std::string cursor_of(const std::string &key, const std::string &instance = "an instance")
	{
		return scan::encode_cursor(key, instance, 7);
	}
}

TEST(scan_test, a_range_of_a_partition_alone_is_the_whole_of_that_partition)
{
	std::expected<scan::range, error::error_message> range = parsed("");

	ASSERT_TRUE(range.has_value());
	EXPECT_EQ(range->partition, 7u);
	EXPECT_FALSE(range->has_from);
	EXPECT_FALSE(range->has_to);
	EXPECT_FALSE(range->reverse);
	EXPECT_TRUE(range->values);
	EXPECT_EQ(range->limit, scan::default_limit);
}

// A client cannot hash a key itself, and the records of one partition key are what a scan is
// usually after, so a key is the other way to name the partition to read.
TEST(scan_test, a_key_names_the_partition_it_is_in)
{
	std::expected<scan::range, error::error_message> range = scan::parse_range("key=user%3A7203", "an instance");

	ASSERT_TRUE(range.has_value());
	EXPECT_EQ(range->partition, cluster::partition_of("user:7203"));
}

// A scan of a whole table is a scan of each of its partitions, so the partition is not something
// the API defaults: a range that names none is a client expecting the table to arrive at once.
TEST(scan_test, fail_to_read_a_range_that_names_no_partition)
{
	std::expected<scan::range, error::error_message> range = scan::parse_range("prefix=user", "an instance");

	ASSERT_FALSE(range.has_value());
	EXPECT_EQ(range.error().code, error::code::invalid_partition);
}

TEST(scan_test, fail_to_read_a_range_that_names_a_partition_two_ways)
{
	std::expected<scan::range, error::error_message> range =
		scan::parse_range("partition=7&key=user%3A7203", "an instance");

	ASSERT_FALSE(range.has_value());
	EXPECT_EQ(range.error().code, error::code::invalid_partition);
}

TEST(scan_test, fail_to_read_a_partition_that_is_not_one_of_them)
{
	EXPECT_EQ(scan::parse_range("partition=256", "an instance").error().code, error::code::invalid_partition);
	EXPECT_EQ(scan::parse_range("partition=-1", "an instance").error().code, error::code::invalid_partition);
	EXPECT_EQ(scan::parse_range("partition=wibble", "an instance").error().code, error::code::invalid_partition);
	EXPECT_EQ(scan::parse_range("partition=7wibble", "an instance").error().code, error::code::invalid_partition);
	EXPECT_TRUE(scan::parse_range("partition=255", "an instance").has_value());
	EXPECT_TRUE(scan::parse_range("partition=0", "an instance").has_value());
}

TEST(scan_test, a_prefix_is_shorthand_for_a_from_and_a_to)
{
	std::expected<scan::range, error::error_message> range = parsed("prefix=user%3A");

	ASSERT_TRUE(range.has_value());
	EXPECT_TRUE(range->has_from);
	EXPECT_EQ(range->from, "user:");
	EXPECT_TRUE(range->has_to);
	EXPECT_EQ(range->to, "user;");
}

TEST(scan_test, a_prefix_of_high_bytes_runs_to_the_end)
{
	std::expected<scan::range, error::error_message> range = parsed("prefix=%FF%FF");

	ASSERT_TRUE(range.has_value());
	EXPECT_TRUE(range->has_from);
	EXPECT_FALSE(range->has_to);
}

TEST(scan_test, from_and_to_are_read)
{
	std::expected<scan::range, error::error_message> range = parsed("from=a&to=b");

	ASSERT_TRUE(range.has_value());
	EXPECT_EQ(range->from, "a");
	EXPECT_EQ(range->to, "b");
}

TEST(scan_test, from_and_to_override_a_prefix)
{
	std::expected<scan::range, error::error_message> range = parsed("prefix=user&from=user%3A1&to=user%3A9");

	ASSERT_TRUE(range.has_value());
	EXPECT_EQ(range->from, "user:1");
	EXPECT_EQ(range->to, "user:9");
}

TEST(scan_test, fail_to_read_a_range_that_is_not_below_its_end)
{
	std::expected<scan::range, error::error_message> range = parsed("from=b&to=a");

	ASSERT_FALSE(range.has_value());
	EXPECT_EQ(range.error().code, error::code::invalid_range);
}

TEST(scan_test, fail_to_read_an_empty_range)
{
	std::expected<scan::range, error::error_message> range = parsed("from=a&to=a");

	ASSERT_FALSE(range.has_value());
	EXPECT_EQ(range.error().code, error::code::invalid_range);
}

TEST(scan_test, the_bounds_keep_their_meaning_when_the_scan_is_reversed)
{
	std::expected<scan::range, error::error_message> range = parsed("from=a&to=b&reverse=true");

	ASSERT_TRUE(range.has_value());
	EXPECT_TRUE(range->reverse);
	EXPECT_EQ(range->from, "a");
	EXPECT_EQ(range->to, "b");
}

TEST(scan_test, values_are_asked_for_by_not_refusing_them)
{
	EXPECT_FALSE(parsed("values=false")->values);
	EXPECT_TRUE(parsed("values=true")->values);
	EXPECT_TRUE(parsed("")->values);
}

TEST(scan_test, a_limit_is_read_and_capped)
{
	EXPECT_EQ(parsed("limit=10")->limit, 10);
	EXPECT_EQ(parsed("limit=100000")->limit, scan::max_limit);
	EXPECT_EQ(parsed("limit=0")->limit, 1);
	EXPECT_EQ(parsed("limit=wibble")->limit, scan::default_limit);
	EXPECT_EQ(parsed("limit=10wibble")->limit, scan::default_limit);
}

TEST(scan_test, a_cursor_resumes_strictly_after_the_last_key)
{
	std::expected<scan::range, error::error_message> range = parsed("cursor=" + cursor_of("user:7203"));

	ASSERT_TRUE(range.has_value());
	EXPECT_TRUE(range->has_from);
	EXPECT_EQ(range->from, std::string("user:7203\0", 10));
}

TEST(scan_test, a_cursor_of_a_reverse_scan_resumes_strictly_before_the_last_key)
{
	std::expected<scan::range, error::error_message> range = parsed("reverse=true&cursor=" + cursor_of("user:7203"));

	ASSERT_TRUE(range.has_value());
	EXPECT_TRUE(range->has_to);
	EXPECT_EQ(range->to, "user:7203");
}

TEST(scan_test, a_cursor_narrows_the_range_it_was_issued_for)
{
	std::expected<scan::range, error::error_message> range = parsed("prefix=user%3A&cursor=" + cursor_of("user:5000"));

	ASSERT_TRUE(range.has_value());
	EXPECT_EQ(range->from, std::string("user:5000\0", 10));
	EXPECT_EQ(range->to, "user;");
}

TEST(scan_test, fail_to_read_a_cursor_from_another_instance)
{
	std::expected<scan::range, error::error_message> range =
		parsed("cursor=" + cursor_of("user:7203", "another instance"));

	ASSERT_FALSE(range.has_value());
	EXPECT_EQ(range.error().code, error::code::invalid_cursor);
}

// A cursor is a position in one partition, and the partition it was issued for is not the one this
// scan reads: the key it names holds nothing here, which is not an empty range but a wrong one.
TEST(scan_test, fail_to_read_a_cursor_issued_for_another_partition)
{
	std::expected<scan::range, error::error_message> range =
		scan::parse_range("partition=8&cursor=" + scan::encode_cursor("user:7203", "an instance", 7), "an instance");

	ASSERT_FALSE(range.has_value());
	EXPECT_EQ(range.error().code, error::code::invalid_cursor);
}

TEST(scan_test, fail_to_read_a_cursor_that_is_not_a_cursor)
{
	EXPECT_EQ(parsed("cursor=not+a+cursor").error().code, error::code::invalid_cursor);
	EXPECT_EQ(parsed("cursor=%7B%22k%22%3A%22a%22%7D").error().code, error::code::invalid_cursor);
}

TEST(scan_test, a_cursor_is_opaque_and_encodes_the_key_it_resumes_after)
{
	std::string cursor = cursor_of("user:7203");

	EXPECT_EQ(cursor.find("user"), std::string::npos);
	EXPECT_EQ(parsed("cursor=" + cursor)->from, std::string("user:7203\0", 10));
}
