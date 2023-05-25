#include <filesystem>
#include <string>

#include <boost/json.hpp>

#include "error.h"
#include "rocksdb_repository.h"

repository::rocksdb_repository::rocksdb_repository()
{
	rocksdb::Options options;
	options.create_if_missing = true;

	std::filesystem::create_directory("/tmp/asyncdb");
	rocksdb::Status status = rocksdb::DB::Open(options, "/tmp/asyncdb/" + std::to_string(std::rand()), &database);

	if (!status.ok())
	{
		throw std::runtime_error(ERROR("Failed to open rocksdb with status: " + status.ToString()));
	}
}

void repository::rocksdb_repository::create_table(const table::table &table)
{
	if (table.is_valid)
	{
		database->Put(rocksdb::WriteOptions(), "TABLE_" + table.name, boost::json::serialize(table.json));
	}
}

std::set<table::table> repository::rocksdb_repository::list_tables() const
{
	rocksdb::ReadOptions options;
	rocksdb::Iterator *it = database->NewIterator(options);
	std::set<table::table> tables;

	for(it->Seek("TABLE"); it->Valid(); it->Next())
	{
		tables.insert(table::to_table(it->value().ToString()));
	}

	return tables;
}

bool repository::rocksdb_repository::has_table(const std::string &table_name) const
{
	std::string value;

	database->Get(rocksdb::ReadOptions(), "TABLE_" + table_name, &value);

	return !value.empty();
}

table::table repository::rocksdb_repository::read_table(const std::string &table_name) const
{
	std::string value;

	database->Get(rocksdb::ReadOptions(), "TABLE_" + table_name, &value);

	if (value.empty())
	{
		return table::invalid_table("No table with name \"" + table_name + "\" found in data store.");
	}
	else
	{
		return table::to_table(value);
	}
}

repository::rocksdb_repository::~rocksdb_repository()
{
	delete database;
}
