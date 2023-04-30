#include <gtest/gtest.h>
#include <boost/json.hpp>

#include "table/table.h"

TEST(table_test, deserialise_and_serialise)
{
	boost::json::object json;
	boost::json::array dependencies;

	dependencies.push_back("dependency one");
	dependencies.push_back("dependency two");

	json.insert(std::pair("name", "table name"));
	json.insert(std::pair("dependencies", dependencies));

	table::table table = table::parse_table(json, std::set<std::string>{"dependency one", "dependency two"});

	ASSERT_TRUE(table.is_valid);
	ASSERT_EQ(boost::json::serialize(table.json), "{\"name\":\"table name\",\"dependencies\":[\"dependency one\",\"dependency two\"]}");
}

TEST(table_test, fail_to_deserialise_table_with_empty_name)
{
	boost::json::object json;
	boost::json::array dependencies;

	dependencies.push_back("dependency one");

	json.insert(std::pair("name", ""));
	json.insert(std::pair("dependencies", dependencies));

	table::table table = table::parse_table(json, std::set<std::string>{});

	ASSERT_FALSE(table.is_valid);
	ASSERT_EQ(boost::json::serialize(table.json), "{\"error\":\"Table requires name of length greater than 0.\"}");
}

TEST(table_test, fail_to_deserialise_table_with_no_name)
{
	boost::json::object json;
	boost::json::array dependencies;

	dependencies.push_back("dependency one");

	json.insert(std::pair("dependencies", dependencies));

	table::table table = table::parse_table(json, std::set<std::string>{});

	ASSERT_FALSE(table.is_valid);
	ASSERT_EQ(boost::json::serialize(table.json), "{\"error\":\"Table requires name of length greater than 0.\"}");
}

TEST(table_test, fail_to_deserialise_table_with_duplicate_name)
{
	boost::json::object json;

	json.insert(std::pair("name", "table name"));
	json.insert(std::pair("dependencies", boost::json::array()));

	table::table table = table::parse_table(json, std::set<std::string>{"table name"});

	ASSERT_FALSE(table.is_valid);
	ASSERT_EQ(boost::json::serialize(table.json), "{\"error\":\"A table with the name \\\"table name\\\" already exists.\"}");
}

TEST(table_test, fail_to_deserialise_table_with_invalid_dependency)
{
	boost::json::object json;
	boost::json::array dependencies;

	dependencies.push_back("dependency one");
	dependencies.push_back("dependency two");

	json.insert(std::pair("name", "table name"));
	json.insert(std::pair("dependencies", dependencies));

	table::table table = table::parse_table(json, std::set<std::string>{"dependency one"});

	ASSERT_FALSE(table.is_valid);
	ASSERT_EQ(boost::json::serialize(table.json), "{\"error\":\"Dependency \\\"dependency two\\\" is not a table.\"}");
}
