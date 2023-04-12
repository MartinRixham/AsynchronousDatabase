#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <boost/json/src.hpp>

#include <memory>
#include <string>
#include <vector>
#include <filesystem>

#include "repository/rocksdb_repository.h"

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
	repository.create_table("first table");
	repository.create_table("second table");

	std::vector<std::string> tables = repository.list_tables();

	ASSERT_EQ(2, tables.size());

	ASSERT_EQ("first table", boost::json::parse(tables[0]).as_object()["name"]);
	ASSERT_EQ("second table", boost::json::parse(tables[1]).as_object()["name"]);
}
