#include <gtest/gtest.h>
#include <boost/json.hpp>

#include "repository/fake_repository.h"
#include "router/router.h"

TEST(router_test, nonsense)
{
	repository::fake_repository repository;
	router::router router(repository);
	boost::json::object request;

	request.insert(std::pair("name", "really a real table"));

	router.post("/wibble", request);

	ASSERT_FALSE(repository.has_table("really a real table"));
}

TEST(router_test, create_table)
{
	repository::fake_repository repository;
	router::router router(repository);
	boost::json::object request;

	request.insert(std::pair("name", "really a real table"));
	request.insert(std::pair("dependencies", boost::json::array()));

	router.post("/table", request);

	ASSERT_TRUE(repository.has_table("really a real table"));
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
	ASSERT_FALSE(repository.has_table(""));
}

TEST(router_test, fail_to_create_duplicate_table)
{
	repository::fake_repository repository;
	router::router router(repository);
	boost::json::object request;

	request.insert(std::pair("name", "table name"));
	request.insert(std::pair("dependencies", boost::json::array()));

	boost::json::object response = router.post("/table", request);

	ASSERT_EQ(response.size(), 0);

	response = router.post("/table", request);

	ASSERT_EQ(response["error"].as_string(), "A table with the name \"table name\" already exists.");
}

TEST(router_test, read_table)
{
	repository::fake_repository repository;
	router::router router(repository);
	boost::json::object first_request;
	boost::json::object second_request;

	first_request.insert(std::pair("name", "first table"));
	first_request.insert(std::pair("dependencies", boost::json::array()));

	second_request.insert(std::pair("name", "second table"));
	second_request.insert(std::pair("dependencies", boost::json::array()));

	router.post("/table", first_request);
	router.post("/table", second_request);

	boost::json::array tables = router.get("/tables")["tables"].as_array();

	ASSERT_EQ(2, tables.size());
	ASSERT_EQ("first table", tables[0].as_object()["name"]);
	ASSERT_EQ("second table", tables[1].as_object()["name"]);
}
