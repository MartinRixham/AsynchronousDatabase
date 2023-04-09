#include <gtest/gtest.h>
#include <boost/json.hpp>

#include "repository/fake_repository.h"
#include "router/router.h"

TEST(router_test, nonsense)
{
	repository::fake_repository repository;
	router::router router(repository);

	router.post("wibble", "really a real table");

	ASSERT_FALSE(repository.has_table("really a real table"));
}

TEST(router_test, create_table)
{
	repository::fake_repository repository;
	router::router router(repository);

	router.post("table", "really a real table");

	ASSERT_TRUE(repository.has_table("really a real table"));
}

TEST(router_test, read_table)
{
	repository::fake_repository repository;
	router::router router(repository);

	router.post("table", "first table");
	router.post("table", "second table");

	boost::json::array tables = router.get("tables")["tables"].as_array();

	ASSERT_EQ(2, tables.size());
	ASSERT_EQ("first table", tables[0]);
	ASSERT_EQ("second table", tables[1]);
}
