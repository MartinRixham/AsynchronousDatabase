#include <gtest/gtest.h>
#include <boost/json.hpp>

#include "table/table.h"

TEST(table_test, deserialise_and_serialise)
{
	boost::json::array dependencies;

	dependencies.push_back("dependency_one");
	dependencies.push_back("dependency_two");

	boost::json::object json { { "dependencies", dependencies } };

	std::expected<table::table, error::error_message> table =
		table::parse_table("a_table", json, std::set<std::string> { "dependency_one", "dependency_two" });

	ASSERT_TRUE(table.has_value());
	EXPECT_EQ(
		boost::json::serialize((*table).to_json()),
		"{\"name\":\"a_table\",\"dependencies\":[\"dependency_one\",\"dependency_two\"]}");
}

TEST(table_test, a_table_with_no_dependencies_is_one_nothing_feeds)
{
	std::expected<table::table, error::error_message> table =
		table::parse_table("a_table", boost::json::object(), std::set<std::string> {});

	ASSERT_TRUE(table.has_value());
	EXPECT_EQ(boost::json::serialize((*table).to_json()), "{\"name\":\"a_table\",\"dependencies\":[]}");
}

TEST(table_test, fail_to_deserialise_table_with_empty_name)
{
	std::expected<table::table, error::error_message> table =
		table::parse_table("", boost::json::object(), std::set<std::string> {});

	ASSERT_FALSE(table.has_value());
	EXPECT_EQ(table.error().code, error::code::invalid_table_name);
}

TEST(table_test, fail_to_deserialise_table_with_invalid_dependency)
{
	boost::json::array dependencies;

	dependencies.push_back("dependency_one");
	dependencies.push_back("dependency_two");

	boost::json::object json { { "dependencies", dependencies } };

	std::expected<table::table, error::error_message> table =
		table::parse_table("a_table", json, std::set<std::string> { "dependency_one" });

	ASSERT_FALSE(table.has_value());
	EXPECT_EQ(table.error().code, error::code::dependency_not_found);
	EXPECT_EQ(table.error().message, "Dependency \"dependency_two\" is not a table.");
}

TEST(table_test, fail_to_deserialise_table_with_a_dependency_that_is_not_a_name)
{
	boost::json::array dependencies;

	dependencies.push_back(boost::json::object { { "name", "dependency_one" } });

	boost::json::object json { { "dependencies", dependencies } };

	std::expected<table::table, error::error_message> table =
		table::parse_table("a_table", json, std::set<std::string> { "dependency_one" });

	ASSERT_FALSE(table.has_value());
	EXPECT_EQ(table.error().code, error::code::dependency_not_found);
}

TEST(table_test, fail_to_deserialise_table_whose_dependencies_are_not_a_list)
{
	boost::json::object json { { "dependencies", "dependency_one" } };

	std::expected<table::table, error::error_message> table =
		table::parse_table("a_table", json, std::set<std::string> { "dependency_one" });

	ASSERT_FALSE(table.has_value());
	EXPECT_EQ(table.error().code, error::code::dependency_not_found);
}

TEST(table_test, valid_names)
{
	EXPECT_TRUE(table::is_valid_name("a"));
	EXPECT_TRUE(table::is_valid_name("a table"));
	EXPECT_TRUE(table::is_valid_name("account"));
	EXPECT_TRUE(table::is_valid_name("Account"));
	EXPECT_TRUE(table::is_valid_name("account_2019-1"));
	EXPECT_TRUE(table::is_valid_name(std::string(table::max_name_size, 'a')));
}

TEST(table_test, invalid_names)
{
	EXPECT_FALSE(table::is_valid_name(""));
	EXPECT_FALSE(table::is_valid_name("account."));
	EXPECT_FALSE(table::is_valid_name(std::string(table::max_name_size + 1, 'a')));

	// The column family RocksDB always has.
	EXPECT_FALSE(table::is_valid_name("default"));
}

TEST(table_test, tables_are_equal_when_their_options_are)
{
	std::vector<std::string> dependencies { "a_table" };

	EXPECT_TRUE((table::table { "another", dependencies }) == (table::table { "another", dependencies }));
	EXPECT_FALSE((table::table { "another", dependencies }) == (table::table { "another", {} }));
	EXPECT_FALSE((table::table { "another", dependencies }) == (table::table { "a_third", dependencies }));
}

// A stored document is the table it was declared as, so declaring it again is the same table.
TEST(table_test, a_stored_table_is_read_back_as_the_table_it_was_declared_as)
{
	table::table declared { "a_table", { "a_dependency" } };

	EXPECT_EQ(table::to_table(declared.to_json()), declared);
}
