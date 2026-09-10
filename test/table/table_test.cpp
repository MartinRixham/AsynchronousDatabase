#include <gtest/gtest.h>
#include <boost/json.hpp>

#include "table/table.h"

TEST(table_test, deserialise_and_serialise)
{
	boost::json::array dependencies;

	dependencies.push_back("dependency_one");
	dependencies.push_back("dependency_two");

	boost::json::object json { { "dependencies", dependencies } };

	table::table table =
		table::parse_table("a_table", json, std::set<std::string> { "dependency_one", "dependency_two" });

	EXPECT_TRUE(table.is_valid);
	EXPECT_EQ(
		boost::json::serialize(table.json),
		"{\"name\":\"a_table\",\"dependencies\":[\"dependency_one\",\"dependency_two\"],\"immutable\":false}");
}

TEST(table_test, a_table_with_no_dependencies_is_one_nothing_feeds)
{
	table::table table = table::parse_table("a_table", boost::json::object(), std::set<std::string> {});

	EXPECT_TRUE(table.is_valid);
	EXPECT_EQ(boost::json::serialize(table.json), "{\"name\":\"a_table\",\"dependencies\":[],\"immutable\":false}");
}

TEST(table_test, fail_to_deserialise_table_with_empty_name)
{
	table::table table = table::parse_table("", boost::json::object(), std::set<std::string> {});

	EXPECT_FALSE(table.is_valid);
	EXPECT_EQ(table.code, "invalid_table_name");
	EXPECT_EQ(table.json.at("error").as_object().at("code"), "invalid_table_name");
}

TEST(table_test, fail_to_deserialise_table_with_invalid_dependency)
{
	boost::json::array dependencies;

	dependencies.push_back("dependency_one");
	dependencies.push_back("dependency_two");

	boost::json::object json { { "dependencies", dependencies } };

	table::table table = table::parse_table("a_table", json, std::set<std::string> { "dependency_one" });

	EXPECT_FALSE(table.is_valid);
	EXPECT_EQ(table.code, "dependency_not_found");
	EXPECT_EQ(table.message, "Dependency \"dependency_two\" is not a table.");
}

TEST(table_test, fail_to_deserialise_table_with_a_dependency_that_is_not_a_name)
{
	boost::json::array dependencies;

	dependencies.push_back(boost::json::object { { "name", "dependency_one" } });

	boost::json::object json { { "dependencies", dependencies } };

	table::table table = table::parse_table("a_table", json, std::set<std::string> { "dependency_one" });

	EXPECT_FALSE(table.is_valid);
	EXPECT_EQ(table.code, "dependency_not_found");
}

TEST(table_test, fail_to_deserialise_table_whose_dependencies_are_not_a_list)
{
	boost::json::object json { { "dependencies", "dependency_one" } };

	table::table table = table::parse_table("a_table", json, std::set<std::string> { "dependency_one" });

	EXPECT_FALSE(table.is_valid);
	EXPECT_EQ(table.code, "dependency_not_found");
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

	EXPECT_TRUE(
		table::valid_table("another", dependencies, false) == table::valid_table("another", dependencies, false));
	EXPECT_FALSE(table::valid_table("another", dependencies, false) == table::valid_table("another", {}, false));
	EXPECT_FALSE(
		table::valid_table("another", dependencies, false) == table::valid_table("a_third", dependencies, false));
	EXPECT_FALSE(
		table::valid_table("another", dependencies, false) == table::valid_table("another", dependencies, true));
}

TEST(table_test, deserialise_a_table_whose_keys_may_be_written_once)
{
	boost::json::object json { { "immutable", true } };

	table::table table = table::parse_table("a_table", json, std::set<std::string> {});

	EXPECT_TRUE(table.is_valid);
	EXPECT_TRUE(table.immutable);
	EXPECT_EQ(boost::json::serialize(table.json), "{\"name\":\"a_table\",\"dependencies\":[],\"immutable\":true}");
}

TEST(table_test, a_table_that_does_not_declare_itself_immutable_is_not)
{
	boost::json::object json { { "immutable", false } };

	EXPECT_FALSE(table::parse_table("a_table", json, std::set<std::string> {}).immutable);
	EXPECT_FALSE(table::parse_table("a_table", boost::json::object(), std::set<std::string> {}).immutable);
}

TEST(table_test, fail_to_deserialise_a_table_whose_immutable_option_is_not_a_boolean)
{
	boost::json::object json { { "immutable", "true" } };

	table::table table = table::parse_table("a_table", json, std::set<std::string> {});

	EXPECT_FALSE(table.is_valid);
	EXPECT_EQ(table.code, "invalid_body");
}

// A stored document carries the options the table was declared with, and an option it does not
// carry is read back as its default — so declaring the table again is the same table.
TEST(table_test, a_stored_table_that_says_nothing_about_immutability_is_mutable)
{
	table::table table = table::to_table("{\"name\":\"a_table\",\"dependencies\":[]}");

	EXPECT_FALSE(table.immutable);
	EXPECT_TRUE(table == table::valid_table("a_table", std::vector<std::string>(), false));
}

TEST(table_test, a_stored_immutable_table_is_read_back_as_one)
{
	table::table table = table::to_table("{\"name\":\"a_table\",\"dependencies\":[],\"immutable\":true}");

	EXPECT_TRUE(table.immutable);
	EXPECT_TRUE(table == table::valid_table("a_table", std::vector<std::string>(), true));
}
