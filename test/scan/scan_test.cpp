#include <string>

#include <gtest/gtest.h>
#include <boost/json.hpp>

#include "scan/scan.h"

TEST(scan_test, a_range_with_no_parameters_is_the_whole_table)
{
	scan::range range = scan::parse_range("", "an instance");

	EXPECT_TRUE(range.is_valid);
	EXPECT_FALSE(range.has_from);
	EXPECT_FALSE(range.has_to);
	EXPECT_FALSE(range.reverse);
	EXPECT_TRUE(range.values);
	EXPECT_EQ(range.limit, scan::default_limit);
}

TEST(scan_test, a_prefix_is_shorthand_for_a_from_and_a_to)
{
	scan::range range = scan::parse_range("prefix=user%3A", "an instance");

	EXPECT_TRUE(range.is_valid);
	EXPECT_TRUE(range.has_from);
	EXPECT_EQ(range.from, "user:");
	EXPECT_TRUE(range.has_to);
	EXPECT_EQ(range.to, "user;");
}

TEST(scan_test, a_prefix_of_high_bytes_runs_to_the_end)
{
	scan::range range = scan::parse_range("prefix=%FF%FF", "an instance");

	EXPECT_TRUE(range.is_valid);
	EXPECT_TRUE(range.has_from);
	EXPECT_FALSE(range.has_to);
}

TEST(scan_test, from_and_to_are_read)
{
	scan::range range = scan::parse_range("from=a&to=b", "an instance");

	EXPECT_TRUE(range.is_valid);
	EXPECT_EQ(range.from, "a");
	EXPECT_EQ(range.to, "b");
}

TEST(scan_test, from_and_to_override_a_prefix)
{
	scan::range range = scan::parse_range("prefix=user&from=user%3A1&to=user%3A9", "an instance");

	EXPECT_EQ(range.from, "user:1");
	EXPECT_EQ(range.to, "user:9");
}

TEST(scan_test, fail_to_read_a_range_that_is_not_below_its_end)
{
	scan::range range = scan::parse_range("from=b&to=a", "an instance");

	EXPECT_FALSE(range.is_valid);
	EXPECT_EQ(range.code, "invalid_range");
}

TEST(scan_test, fail_to_read_an_empty_range)
{
	scan::range range = scan::parse_range("from=a&to=a", "an instance");

	EXPECT_FALSE(range.is_valid);
	EXPECT_EQ(range.code, "invalid_range");
}

TEST(scan_test, the_bounds_keep_their_meaning_when_the_scan_is_reversed)
{
	scan::range range = scan::parse_range("from=a&to=b&reverse=true", "an instance");

	EXPECT_TRUE(range.is_valid);
	EXPECT_TRUE(range.reverse);
	EXPECT_EQ(range.from, "a");
	EXPECT_EQ(range.to, "b");
}

TEST(scan_test, values_are_asked_for_by_not_refusing_them)
{
	EXPECT_FALSE(scan::parse_range("values=false", "an instance").values);
	EXPECT_TRUE(scan::parse_range("values=true", "an instance").values);
	EXPECT_TRUE(scan::parse_range("", "an instance").values);
}

TEST(scan_test, a_limit_is_read_and_capped)
{
	EXPECT_EQ(scan::parse_range("limit=10", "an instance").limit, 10);
	EXPECT_EQ(scan::parse_range("limit=100000", "an instance").limit, scan::max_limit);
	EXPECT_EQ(scan::parse_range("limit=0", "an instance").limit, 1);
	EXPECT_EQ(scan::parse_range("limit=wibble", "an instance").limit, scan::default_limit);
	EXPECT_EQ(scan::parse_range("limit=10wibble", "an instance").limit, scan::default_limit);
}

TEST(scan_test, a_cursor_resumes_strictly_after_the_last_key)
{
	std::string cursor = scan::encode_cursor("user:7203", "an instance");
	scan::range range = scan::parse_range("cursor=" + cursor, "an instance");

	EXPECT_TRUE(range.is_valid);
	EXPECT_TRUE(range.has_from);
	EXPECT_EQ(range.from, std::string("user:7203\0", 10));
}

TEST(scan_test, a_cursor_of_a_reverse_scan_resumes_strictly_before_the_last_key)
{
	std::string cursor = scan::encode_cursor("user:7203", "an instance");
	scan::range range = scan::parse_range("reverse=true&cursor=" + cursor, "an instance");

	EXPECT_TRUE(range.is_valid);
	EXPECT_TRUE(range.has_to);
	EXPECT_EQ(range.to, "user:7203");
}

TEST(scan_test, a_cursor_narrows_the_range_it_was_issued_for)
{
	std::string cursor = scan::encode_cursor("user:5000", "an instance");
	scan::range range = scan::parse_range("prefix=user%3A&cursor=" + cursor, "an instance");

	EXPECT_TRUE(range.is_valid);
	EXPECT_EQ(range.from, std::string("user:5000\0", 10));
	EXPECT_EQ(range.to, "user;");
}

TEST(scan_test, fail_to_read_a_cursor_from_another_instance)
{
	std::string cursor = scan::encode_cursor("user:7203", "another instance");
	scan::range range = scan::parse_range("cursor=" + cursor, "an instance");

	EXPECT_FALSE(range.is_valid);
	EXPECT_EQ(range.code, "invalid_cursor");
}

TEST(scan_test, fail_to_read_a_cursor_that_is_not_a_cursor)
{
	EXPECT_EQ(scan::parse_range("cursor=not+a+cursor", "an instance").code, "invalid_cursor");
	EXPECT_EQ(scan::parse_range("cursor=%7B%22k%22%3A%22a%22%7D", "an instance").code, "invalid_cursor");
}

TEST(scan_test, a_cursor_is_opaque_and_encodes_the_key_it_resumes_after)
{
	std::string cursor = scan::encode_cursor("user:7203", "an instance");

	EXPECT_EQ(cursor.find("user"), std::string::npos);
	EXPECT_EQ(scan::parse_range("cursor=" + cursor, "an instance").from, std::string("user:7203\0", 10));
}
