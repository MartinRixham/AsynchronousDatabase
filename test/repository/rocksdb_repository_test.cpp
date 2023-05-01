#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <boost/json/src.hpp>

#include <set>
#include <vector>
#include <filesystem>

#include "repository/rocksdb_repository.h"
#include "table/table.h"

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

	std::set<table::table> table_set = repository.list_tables();
	std::vector<table::table> tables(table_set.begin(), table_set.end());

	ASSERT_EQ(tables.size(), 2);

	ASSERT_EQ(tables[0].name, "first table");
	ASSERT_EQ(tables[1].name, "second table");
}

TEST_F(repository_test, does_not_have_invalid_table)
{
	repository.create_table(table::invalid_table("error"));

	std::set<table::table> tables = repository.list_tables();

	ASSERT_EQ(tables.size(), 0);
}

TEST_F(repository_test, has_valid_table)
{
	repository.create_table(table::valid_table("a table", std::vector<std::string>()));

	bool has_table = repository.has_table(table::valid_table("a table", std::vector<std::string>()));

	ASSERT_TRUE(has_table);
}
