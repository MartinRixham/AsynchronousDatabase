#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <boost/json/src.hpp>

#include <memory>
#include <string>
#include <vector>
#include <filesystem>

#include "repository/rocksdb_repository.h"
#include "table/valid_table.h"
#include "table/invalid_table.h"

class repository_test: public ::testing::Test
{ 
protected:
	repository::rocksdb_repository repository;

	void SetUp()
	{
	}

	void TearDown()
	{
	}
};

TEST_F(repository_test, read_tables)
{
	repository.create_table(table::valid_table("first table", std::vector<std::string>()));
	repository.create_table(table::valid_table("second table", std::vector<std::string>()));

	std::vector<std::string> tables = repository.list_tables();

	ASSERT_EQ(tables.size(), 2);

	ASSERT_EQ(boost::json::parse(tables[0]).as_object()["name"], "first table");
	ASSERT_EQ(boost::json::parse(tables[1]).as_object()["name"], "second table");
}

TEST_F(repository_test, does_not_have_invalid_table_table)
{
	repository.create_table(table::invalid_table("errro"));

	std::vector<std::string> tables = repository.list_tables();

	ASSERT_EQ(tables.size(), 0);
}

TEST_F(repository_test, has_valid_table_table)
{
	repository.create_table(table::valid_table("a table", std::vector<std::string>()));

	bool has_table = repository.has_table(table::valid_table("a table", std::vector<std::string>()));

	ASSERT_TRUE(has_table);
}
