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

	std::filesystem::path database_path = std::filesystem::path(directory);
	std::filesystem::create_directories(database_path);

	instance_name = std::to_string(std::rand());

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

		written(
			database->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), table.name, &handle),
			"Creating table \"" + table.name + "\"");

		handles.insert({ table.name, handle });
	}

	written(
		database->Put(rocksdb::WriteOptions(), table_prefix + table.name, boost::json::serialize(table.json)),
		"Writing table \"" + table.name + "\"");
}

// The table documents are in the default column family, which is open for the life of the store
// and named by no handle, so reading one takes no lock. Guarding it would put every table read
// behind the exclusive lock a table create holds.
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

	written(database->DropColumnFamily(handle->second), "Dropping table \"" + table_name + "\"");

	database->DestroyColumnFamilyHandle(handle->second);
	handles.erase(handle);

	written(
		database->Delete(rocksdb::WriteOptions(), table_prefix + table_name),
		"Deleting table \"" + table_name + "\"");
}

void repository::rocksdb_repository::write_record(const std::string &table_name, const record::record &record)
{
	std::shared_lock<std::shared_mutex> lock(handle_mutex);

	written(
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

	written(
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
	size_t bytes = 0;

	for (range.reverse ? it->SeekToLast() : it->SeekToFirst(); it->Valid(); range.reverse ? it->Prev() : it->Next())
	{
		if (page.records.size() == range.limit)
		{
			page.has_more = true;
			break;
		}

		// The size is asked of the slices rather than of strings copied out of them, so a record
		// the budget refuses is one this never allocated.
		size_t size = it->key().size() + (range.values ? it->value().size() : 0);

		if (!page.records.empty() && bytes + size > scan::max_page_bytes)
		{
			page.has_more = true;
			break;
		}

		bytes += size;

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
		written(database->DeleteRange(rocksdb::WriteOptions(), handle, range.from, range.to), what);

		return;
	}

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

	written(database->DeleteRange(rocksdb::WriteOptions(), handle, range.from, last), what);
	written(database->Delete(rocksdb::WriteOptions(), handle, last), what);
}

// Every write goes through here rather than through check() alone. A write that failed for want of
// space leaves RocksDB refusing every write after it: the background error is sticky, and a disk
// with room on it again is not something the store notices by itself. The next write is what asks
// it to look, which is what makes doc/runbook/storage.md true.
//
// Back pressure is not that. Incomplete, Busy and TryAgain are a store that is working and saying
// so, and there is nothing there to resume.
void repository::rocksdb_repository::written(const rocksdb::Status &status, const std::string &what)
{
	if (!status.ok() && !status.IsIncomplete() && !status.IsBusy() && !status.IsTryAgain())
	{
		// Nothing at all when the database was never stopped, and what it cannot clear it leaves:
		// check says what the status was either way, and the write that failed still failed.
		database->Resume();
	}

	check(status, what);
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
