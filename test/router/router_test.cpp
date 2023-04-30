#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <vector>
#include <string>

#include "repository/fake_repository.h"
#include "router/router.h"
#include "table/table.h"

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

	router::response response = router.post("/table", request);

	ASSERT_EQ(response.status, boost::beast::http::status::bad_request);
	ASSERT_EQ(response.body["error"].as_string(), "Table requires name of length greater than 0.");
}

TEST(router_test, fail_to_create_duplicate_table)
{
	repository::fake_repository repository;
	router::router router(repository);
	boost::json::object request;

	request.insert(std::pair("name", "table name"));
	request.insert(std::pair("dependencies", boost::json::array()));

	router.post("/table", request);

	router::response response = router.post("/table", request);

	ASSERT_EQ(response.status, boost::beast::http::status::bad_request);
	ASSERT_EQ(response.body["error"].as_string(), "A table with the name \"table name\" already exists.");
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

	router::response response = router.get("/tables");

	ASSERT_EQ(response.status, boost::beast::http::status::ok);

	boost::json::array tables = response.body["tables"].as_array();

	ASSERT_EQ(tables.size(), 2);
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

	router::response response = router.post("/table", second_request);

	ASSERT_EQ(response.status, boost::beast::http::status::bad_request);
	ASSERT_EQ(response.body["error"], "Dependency \"not a table\" is not a table.");

	boost::json::array tables = router.get("/tables").body["tables"].as_array();

	ASSERT_EQ(tables.size(), 1);
	ASSERT_EQ(tables[0].as_object()["name"], "first table");
}
