#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <iostream>

#include "repository/fake_repository.h"
#include "router/router.h"

TEST(router_test, nonsense)
{
	repository::fake_repository repository;
	router::router router(repository);

	router.post("/wibble", "really a real table");

	ASSERT_FALSE(repository.has_table("really a real table"));
}

TEST(router_test, create_table)
{
	repository::fake_repository repository;
	router::router router(repository);

	router.post("/table", "{ \"name\": \"really a real table\" }");

	std::vector<std::string> tables = repository.list_tables();

	std::cerr << "table count: " << tables.size() << "\n";

	ASSERT_TRUE(repository.has_table("really a real table"));
}

TEST(router_test, read_table)
{
	repository::fake_repository repository;
	router::router router(repository);

	router.post("/table", "{ \"name\": \"first table\" }");
	router.post("/table", "{ \"name\": \"second table\" }");

	boost::json::array tables = router.get("/tables")["tables"].as_array();

	ASSERT_EQ(2, tables.size());
	ASSERT_EQ("first table", tables[0].as_object()["name"]);
	ASSERT_EQ("second table", tables[1].as_object()["name"]);
}
