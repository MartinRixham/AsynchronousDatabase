#include <gtest/gtest.h>
#include <boost/json.hpp>

#include "table/table.h"

TEST(table_test, deserialise_and_serialise)
{
	boost::json::array dependencies;

	dependencies.push_back("dependency_one");
	dependencies.push_back("dependency_two");

	boost::json::object json { { "dependencies", dependencies } };

	table::table table = table::parse_table("a_table", json, std::set<std::string> { "dependency_one", "dependency_two" });

	EXPECT_TRUE(table.is_valid);
	EXPECT_EQ(
		boost::json::serialize(table.json),
		"{\"name\":\"a_table\",\"dependencies\":[\"dependency_one\",\"dependency_two\"]}");
}

TEST(table_test, a_table_with_no_dependencies_is_one_nothing_feeds)
{
	table::table table = table::parse_table("a_table", boost::json::object(), std::set<std::string> {});

	EXPECT_TRUE(table.is_valid);
	EXPECT_EQ(boost::json::serialize(table.json), "{\"name\":\"a_table\",\"dependencies\":[]}");
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
	EXPECT_TRUE(table::is_valid_name("account"));
	EXPECT_TRUE(table::is_valid_name("account_2019-1"));
	EXPECT_TRUE(table::is_valid_name(std::string(table::max_name_size, 'a')));
}

TEST(table_test, invalid_names)
{
	EXPECT_FALSE(table::is_valid_name(""));
	EXPECT_FALSE(table::is_valid_name("Account"));
	EXPECT_FALSE(table::is_valid_name("a table"));
	EXPECT_FALSE(table::is_valid_name("account."));
	EXPECT_FALSE(table::is_valid_name(std::string(table::max_name_size + 1, 'a')));

	// The column family RocksDB always has.
	EXPECT_FALSE(table::is_valid_name("default"));
}

TEST(table_test, tables_are_equal_when_their_options_are)
{
	std::vector<std::string> dependencies { "a_table" };

	EXPECT_TRUE(table::valid_table("another", dependencies) == table::valid_table("another", dependencies));
	EXPECT_FALSE(table::valid_table("another", dependencies) == table::valid_table("another", {}));
	EXPECT_FALSE(table::valid_table("another", dependencies) == table::valid_table("a_third", dependencies));
}
