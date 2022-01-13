#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <boost/json/src.hpp>

#include <memory>

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
