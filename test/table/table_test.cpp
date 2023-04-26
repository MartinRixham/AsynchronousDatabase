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

	table::table table = table::parseTable(json);

	ASSERT_EQ(boost::json::serialize(table.toJson()), "{\"name\":\"table name\",\"dependencies\":[\"dependency one\",\"dependency two\"]}");
}
