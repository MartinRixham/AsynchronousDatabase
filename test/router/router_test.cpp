#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <vector>
#include <string>

#include "repository/fake_repository.h"
#include "router/router.h"
#include "table/valid_table.h"

TEST(router_test, nonsense)
{
	repository::fake_repository repository;
	router::router router(repository);
	boost::json::object request;

	request.insert(std::pair("name", "really a real table"));

	router.post("/wibble", request);

	ASSERT_FALSE(repository.has_table(table::valid_table("really a real table", std::vector<std::string>{})));
}

TEST(router_test, create_table)
{
	repository::fake_repository repository;
	router::router router(repository);
	boost::json::object request;

	request.insert(std::pair("name", "really a real table"));
	request.insert(std::pair("dependencies", boost::json::array()));

	router.post("/table", request);

	ASSERT_TRUE(repository.has_table(table::valid_table("really a real table", std::vector<std::string>{})));
}

TEST(router_test, fail_to_create_table_with_empty_name)
{
	repository::fake_repository repository;
	router::router router(repository);
	boost::json::object request;

	request.insert(std::pair("name", ""));
	request.insert(std::pair("dependencies", boost::json::array()));

	boost::json::object response = router.post("/table", request);

	ASSERT_EQ("Table requires name of length greater than 0.", response["error"].as_string());
}

TEST(router_test, fail_to_create_duplicate_table)
{
	repository::fake_repository repository;
	router::router router(repository);
	boost::json::object request;

	request.insert(std::pair("name", "table name"));
	request.insert(std::pair("dependencies", boost::json::array()));

	router.post("/table", request);

	boost::json::object response = router.post("/table", request);

	ASSERT_EQ(response["error"].as_string(), "A table with the name \"table name\" already exists.");
}

TEST(router_test, read_table)
{
	repository::fake_repository repository;
	router::router router(repository);
	boost::json::object first_request;

	first_request.insert(std::pair("name", "first table"));
	first_request.insert(std::pair("dependencies", boost::json::array()));

	router.post("/table", first_request);

	boost::json::object second_request;

	second_request.insert(std::pair("name", "second table"));
	
	boost::json::array dependencies;

	dependencies.push_back("first table");
	second_request.insert(std::pair("dependencies", dependencies));

	router.post("/table", second_request);

	boost::json::array tables = router.get("/tables")["tables"].as_array();

	ASSERT_EQ(2, tables.size());
	ASSERT_EQ(tables[0].as_object()["name"], "first table");

	boost::json::object second_table = tables[1].as_object();

	ASSERT_EQ(second_table["name"], "second table");
	ASSERT_EQ(second_table["dependencies"].as_array()[0], "first table");
}

TEST(router_test, fail_to_create_table_with_invalid_dependency)
{
	repository::fake_repository repository;
	router::router router(repository);
	boost::json::object first_request;
	boost::json::object second_request;

	first_request.insert(std::pair("name", "first table"));
	first_request.insert(std::pair("dependencies", boost::json::array()));

	router.post("/table", first_request);

	second_request.insert(std::pair("name", "second table"));
	
	boost::json::array dependencies;

	dependencies.push_back("not a table");

	second_request.insert(std::pair("dependencies", dependencies));

	boost::json::object response = router.post("/table", second_request);

	ASSERT_EQ(response["error"], "Dependency \"not a table\" is not a table.");

	boost::json::array tables = router.get("/tables")["tables"].as_array();

	ASSERT_EQ(1, tables.size());
	ASSERT_EQ(tables[0].as_object()["name"], "first table");
}
