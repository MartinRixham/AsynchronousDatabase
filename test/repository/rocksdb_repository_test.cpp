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
	rocksdb::DB *database;

	void SetUp()
	{
	}

	void TearDown()
	{
		delete database;
		std::filesystem::remove_all("/tmp/testdb");
	}
};

TEST_F(repository_test, open_database)
{
	rocksdb::Options options;

	options.create_if_missing = true;

	rocksdb::Status status = rocksdb::DB::Open(options, "/tmp/testdb", &database);

	ASSERT_TRUE(status.ok());
}

TEST_F(repository_test, create_table)
{
	rocksdb::Options options;

	options.create_if_missing = true;

	rocksdb::DB::Open(options, "/tmp/testdb", &database);

	repository::rocksdb_repository repository(database);
	std::string table_name("wot a table");

	repository.create_table(table_name);

	std::string value;
	rocksdb::Status status = database->Get(rocksdb::ReadOptions(), "TABLE_" + table_name, &value);
	boost::json::object json = boost::json::parse(value).as_object();

	ASSERT_EQ(status, rocksdb::Status::OK());
	ASSERT_EQ("wot a table", json["name"]);
}

TEST_F(repository_test, read_tables)
{
	rocksdb::Options options;

	options.create_if_missing = true;

	rocksdb::DB::Open(options, "/tmp/testdb", &database);

	repository::rocksdb_repository repository(database);

	repository.create_table("first table");
	repository.create_table("second table");

	std::vector<std::string> tables = repository.list_tables();

	ASSERT_EQ(2, tables.size());

	ASSERT_EQ("first table", boost::json::parse(tables[0]).as_object()["name"]);
	ASSERT_EQ("second table", boost::json::parse(tables[1]).as_object()["name"]);
}
