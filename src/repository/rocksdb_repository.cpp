#include <filesystem>
#include <string>

#include <boost/json.hpp>

#include "error.h"
#include "rocksdb_repository.h"

namespace
{
	const std::string table_prefix = "TABLE_";

	// RocksDB slows writers down when memtables or level zero back up. That is back pressure and
	// not a failure, so it is told apart from an error the client can do nothing about.
	void check(const rocksdb::Status &status, const std::string &what)
	{
		if (status.ok())
		{
			return;
		}

		if (status.IsIncomplete() || status.IsBusy() || status.IsTryAgain())
		{
			throw repository::storage_error("write_stalled", what + " was refused: " + status.ToString());
		}

		throw repository::storage_error("storage_error", what + " failed: " + status.ToString());
	}

	std::vector<rocksdb::ColumnFamilyDescriptor> describe(const std::vector<std::string> &names)
	{
		std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;

		for (size_t i = 0; i < names.size(); i++)
		{
			descriptors.push_back(rocksdb::ColumnFamilyDescriptor(names[i], rocksdb::ColumnFamilyOptions()));
		}

		return descriptors;
	}
}

repository::rocksdb_repository::rocksdb_repository(const std::string &directory)
{
	rocksdb::Options options;
	options.create_if_missing = true;

	std::filesystem::path directory_path = std::filesystem::path(directory);
	std::filesystem::create_directories(directory_path);

	instance_name = std::to_string(std::rand());

	std::filesystem::path database_path = directory_path / instance_name;
	std::vector<std::string> names;

	rocksdb::DB::ListColumnFamilies(options, database_path.string(), &names);

	if (names.empty())
	{
		names.push_back(rocksdb::kDefaultColumnFamilyName);
	}

	std::vector<rocksdb::ColumnFamilyHandle *> handle_list;
	rocksdb::Status status = rocksdb::DB::Open(options, database_path.string(), describe(names), &handle_list, &database);

	if (!status.ok())
	{
		throw std::runtime_error(ERROR("Failed to open rocksdb with status: " + status.ToString()));
	}

	for (size_t i = 0; i < names.size(); i++)
	{
		handles.insert({ names[i], handle_list[i] });
	}
}

repository::rocksdb_repository::~rocksdb_repository()
{
	// Every column family has to be closed before the database is, and the database outlives this
	// body because it is destroyed with the members afterwards.
	for (std::map<std::string, rocksdb::ColumnFamilyHandle *>::iterator it = handles.begin();
		it != handles.end();
		++it)
	{
		database->DestroyColumnFamilyHandle(it->second);
	}

	handles.clear();
}

void repository::rocksdb_repository::create_table(const table::table &table)
{
	if (!table.is_valid)
	{
		return;
	}

	std::unique_lock<std::shared_mutex> lock(handle_mutex);

	if (handles.find(table.name) == handles.end())
	{
		rocksdb::ColumnFamilyHandle *handle = NULL;

		check(
			database->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), table.name, &handle),
			"Creating table \"" + table.name + "\"");

		handles.insert({ table.name, handle });
	}

	check(
		database->Put(rocksdb::WriteOptions(), table_prefix + table.name, boost::json::serialize(table.json)),
		"Writing table \"" + table.name + "\"");
}

std::set<table::table> repository::rocksdb_repository::list_tables() const
{
	std::unique_ptr<rocksdb::Iterator> it(database->NewIterator(rocksdb::ReadOptions()));
	std::set<table::table> tables;

	for (it->Seek(table_prefix); it->Valid() && it->key().starts_with(table_prefix); it->Next())
	{
		tables.insert(table::to_table(it->value().ToString()));
	}

	return tables;
}

bool repository::rocksdb_repository::has_table(const std::string &table_name) const
{
	std::string value;

	return database->Get(rocksdb::ReadOptions(), table_prefix + table_name, &value).ok();
}

table::table repository::rocksdb_repository::read_table(const std::string &table_name) const
{
	std::string value;

	if (!database->Get(rocksdb::ReadOptions(), table_prefix + table_name, &value).ok())
	{
		return table::invalid_table("table_not_found", "No table named \"" + table_name + "\".");
	}

	return table::to_table(value);
}

void repository::rocksdb_repository::delete_table(const std::string &table_name)
{
	std::unique_lock<std::shared_mutex> lock(handle_mutex);
	std::map<std::string, rocksdb::ColumnFamilyHandle *>::iterator handle = handles.find(table_name);

	if (handle == handles.end())
	{
		return;
	}

	// Dropping the column family takes the data with it: no tombstones and no wait for compaction.
	check(database->DropColumnFamily(handle->second), "Dropping table \"" + table_name + "\"");

	database->DestroyColumnFamilyHandle(handle->second);
	handles.erase(handle);

	check(
		database->Delete(rocksdb::WriteOptions(), table_prefix + table_name),
		"Deleting table \"" + table_name + "\"");
}

void repository::rocksdb_repository::write_record(const std::string &table_name, const record::record &record)
{
	std::shared_lock<std::shared_mutex> lock(handle_mutex);

	check(
		database->Put(rocksdb::WriteOptions(), table_handle(table_name), record.key, record.value),
		"Writing a record to \"" + table_name + "\"");
}

std::optional<std::string> repository::rocksdb_repository::read_record(
	const std::string &table_name,
	const std::string &key) const
{
	std::shared_lock<std::shared_mutex> lock(handle_mutex);
	std::string value;
	rocksdb::Status status = database->Get(rocksdb::ReadOptions(), table_handle(table_name), key, &value);

	// The empty string is a value like any other, so a missing key is told apart by the status
	// and not by the value that comes back with it.
	if (status.IsNotFound())
	{
		return std::nullopt;
	}

	check(status, "Reading a record from \"" + table_name + "\"");

	return value;
}

void repository::rocksdb_repository::delete_record(const std::string &table_name, const std::string &key)
{
	std::shared_lock<std::shared_mutex> lock(handle_mutex);

	check(
		database->Delete(rocksdb::WriteOptions(), table_handle(table_name), key),
		"Deleting a record from \"" + table_name + "\"");
}

scan::page repository::rocksdb_repository::scan_records(const std::string &table_name, const scan::range &range) const
{
	std::shared_lock<std::shared_mutex> lock(handle_mutex);
	rocksdb::ReadOptions options;
	rocksdb::Slice lower(range.from);
	rocksdb::Slice upper(range.to);

	if (range.has_from)
	{
		options.iterate_lower_bound = &lower;
	}

	if (range.has_to)
	{
		options.iterate_upper_bound = &upper;
	}

	std::unique_ptr<rocksdb::Iterator> it(database->NewIterator(options, table_handle(table_name)));
	scan::page page;

	// One more than the page is read, because whether there is a next page is the difference
	// between a cursor and no cursor, and the API promises that no cursor means exhausted.
	for (range.reverse ? it->SeekToLast() : it->SeekToFirst(); it->Valid(); range.reverse ? it->Prev() : it->Next())
	{
		if (page.records.size() == range.limit)
		{
			page.has_more = true;
			break;
		}

		// Not asking for the value lets the iterator stay in the index blocks, which for a table
		// of large values is the whole saving.
		page.records.push_back(
			record::valid_record(it->key().ToString(), range.values ? it->value().ToString() : ""));
	}

	check(it->status(), "Scanning \"" + table_name + "\"");

	return page;
}

void repository::rocksdb_repository::delete_records(const std::string &table_name, const scan::range &range)
{
	std::shared_lock<std::shared_mutex> lock(handle_mutex);
	rocksdb::ColumnFamilyHandle *handle = table_handle(table_name);
	std::string what = "Deleting a range of \"" + table_name + "\"";

	if (range.has_to)
	{
		check(database->DeleteRange(rocksdb::WriteOptions(), handle, range.from, range.to), what);

		return;
	}

	// A range with no upper bound ends at the last key there is, and a range tombstone is half
	// open, so the last key is deleted on its own.
	rocksdb::ReadOptions options;
	rocksdb::Slice lower(range.from);

	if (range.has_from)
	{
		options.iterate_lower_bound = &lower;
	}

	std::unique_ptr<rocksdb::Iterator> it(database->NewIterator(options, handle));

	it->SeekToLast();
	check(it->status(), what);

	if (!it->Valid())
	{
		return;
	}

	std::string last = it->key().ToString();

	check(database->DeleteRange(rocksdb::WriteOptions(), handle, range.from, last), what);
	check(database->Delete(rocksdb::WriteOptions(), handle, last), what);
}

bool repository::rocksdb_repository::is_write_stalled() const
{
	uint64_t stopped = 0;
	uint64_t delayed = 0;

	database->GetIntProperty(rocksdb::DB::Properties::kIsWriteStopped, &stopped);
	database->GetIntProperty(rocksdb::DB::Properties::kActualDelayedWriteRate, &delayed);

	return stopped > 0 || delayed > 0;
}

std::string repository::rocksdb_repository::instance() const
{
	return instance_name;
}

rocksdb::ColumnFamilyHandle *repository::rocksdb_repository::table_handle(const std::string &table_name) const
{
	std::map<std::string, rocksdb::ColumnFamilyHandle *>::const_iterator handle = handles.find(table_name);

	if (handle == handles.end())
	{
		throw storage_error("table_not_found", "No table named \"" + table_name + "\".");
	}

	return handle->second;
}
