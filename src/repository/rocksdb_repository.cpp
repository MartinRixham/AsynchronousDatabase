#include <algorithm>
#include <filesystem>
#include <utility>
#include <string>
#include <vector>

#include <boost/json.hpp>
#include <boost/lexical_cast/try_lexical_convert.hpp>
#include <rocksdb/convenience.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/sst_file_reader.h>
#include <rocksdb/sst_file_writer.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

#include "error.h"
#include "scratch_file.h"
#include "rocksdb_repository.h"

namespace
{
	// The whole schema, in one record of the default column family: every name the store has
	// heard of, live or dropped, each at the version it was last written at. One record rather
	// than one apiece is what lets a node ask another for the schema and take it whole.
	const std::string schema_key = "SCHEMA";

	// What says the values in this store carry a version. A store written before they did holds
	// neither this nor a version on any value, and its values cannot be told from versioned ones.
	const std::string format_key = "FORMAT";

	// 1 is the schema in one versioned record, which is the only format there has ever been.
	const std::string format_version = "1";

	// Beside the table documents in the default column family, and named so that it is no table's
	// document: what a block of write counts has been reserved up to.
	const std::string count_key = "COUNT";

	// How many counts a reservation takes. A write is stamped out of the block held in memory, so
	// this is how many writes there are between the two that cost a record of their own — and how
	// many counts are given up when a process ends, which nothing but the size of the number
	// limits.
	constexpr uint64_t counts_per_reservation = 1024 * 1024;

	const std::string transfer_prefix = "transfer";

	constexpr size_t mebibyte = 1024 * 1024;

	// Three quarters of the budget to the block cache and a quarter to the memtables. Reads are
	// what a cache miss costs a client; a memtable only has to be large enough that a flush is
	// worth doing.
	constexpr size_t cache_share = 4;

	// A memtable each, capped so that a store of many tables is not a store of many memtables: a
	// table is a column family, and a fixed size apiece is memory that grows with the schema.
	constexpr size_t largest_write_buffer = 64 * mebibyte;

	constexpr size_t smallest_write_buffer = 8 * mebibyte;

	// A file open and a table reader for every file is what an unbounded reader cache costs, and
	// a table reader holds index and filter blocks outside the block cache — which is the budget
	// the cache is there to be.
	constexpr int open_files = 1024;

	// The usual trade: about one read in a hundred goes to a file that does not hold the key.
	constexpr double filter_bits = 10;

	// Small enough that a partitioned index or filter is read a block at a time rather than
	// whole, which is what keeps a file's metadata off the heap for a store larger than memory.
	constexpr uint64_t metadata_block_size = 4 * 1024;

	bool supports(rocksdb::CompressionType compression)
	{
		const std::vector<rocksdb::CompressionType> &supported = rocksdb::GetSupportedCompressions();

		return std::find(supported.begin(), supported.end(), compression) != supported.end();
	}

	// What a level compaction rewrites again and again, so it is the cheap one. The store is
	// opened with whatever the build was linked against rather than with a fixed choice: a
	// compression this binary does not have is a store it cannot read back.
	rocksdb::CompressionType compression()
	{
		if (supports(rocksdb::kLZ4Compression))
		{
			return rocksdb::kLZ4Compression;
		}

		return supports(rocksdb::kSnappyCompression) ? rocksdb::kSnappyCompression : rocksdb::kNoCompression;
	}

	// The last level is most of the store and is written once, so it is worth the slower
	// compression that a level being rewritten hourly is not.
	rocksdb::CompressionType bottom_compression()
	{
		return supports(rocksdb::kZSTD) ? rocksdb::kZSTD : compression();
	}

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

	std::vector<rocksdb::ColumnFamilyDescriptor> describe(
		const std::vector<std::string> &names,
		const rocksdb::ColumnFamilyOptions &family_options)
	{
		std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;

		for (size_t i = 0; i < names.size(); i++)
		{
			descriptors.push_back(rocksdb::ColumnFamilyDescriptor(names[i], family_options));
		}

		return descriptors;
	}
}

repository::rocksdb_repository::rocksdb_repository(const std::string &directory, size_t memory_bytes)
{
	rocksdb::LRUCacheOptions cache_options;

	cache_options.capacity = memory_bytes - memory_bytes / cache_share;

	block_cache = cache_options.MakeSharedCache();

	rocksdb::BlockBasedTableOptions table_options;

	table_options.block_cache = block_cache;
	table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(filter_bits));

	// A two level index and a partitioned filter are read a block at a time. A store holding a
	// share of a terabyte is thousands of files, and one index block and one filter block a file
	// is metadata alone larger than the machine.
	table_options.index_type = rocksdb::BlockBasedTableOptions::kTwoLevelIndexSearch;
	table_options.partition_filters = true;
	table_options.metadata_block_size = metadata_block_size;

	// Which only bounds anything if the blocks are charged to the cache like any other. What is
	// pinned instead is the top level of each, because it is one block a file and every read of
	// that file goes through it.
	table_options.cache_index_and_filter_blocks = true;
	table_options.cache_index_and_filter_blocks_with_high_priority = true;
	table_options.pin_top_level_index_and_filter = true;

	size_t memtable_bytes = memory_bytes / cache_share;

	family_options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
	family_options.compression = compression();
	family_options.bottommost_compression = bottom_compression();
	family_options.write_buffer_size =
		std::clamp(memtable_bytes / cache_share, smallest_write_buffer, largest_write_buffer);

	rocksdb::Options options(rocksdb::DBOptions(), family_options);

	options.create_if_missing = true;
	options.max_open_files = open_files;

	// The cap the per table memtables are held under together. Without it the store's memory is
	// the write buffer times however many tables somebody declared.
	options.db_write_buffer_size = memtable_bytes;

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
	rocksdb::Status status =
		rocksdb::DB::Open(options, database_path.string(), describe(names, family_options), &handle_list, &database);

	if (!status.ok())
	{
		throw std::runtime_error(ERROR("Failed to open rocksdb with status: " + status.ToString()));
	}

	for (size_t i = 0; i < names.size(); i++)
	{
		handles.insert({ names[i], handle_list[i] });
	}

	// After the store is open, and emptied every time: what is in here is a transfer that was in
	// flight when the process before this one went, and there is nothing to resume it with.
	transfer_directory = database_path / transfer_prefix;

	std::error_code ignored;

	std::filesystem::remove_all(transfer_directory, ignored);
	std::filesystem::create_directories(transfer_directory);

	// A constructor that throws is an object that is never destroyed, so the handles it opened are
	// given back here rather than by the destructor that will not run.
	try
	{
		check_format();
		read_tables();
	}
	catch (...)
	{
		close_handles();

		throw;
	}
}

void repository::rocksdb_repository::check_format()
{
	std::string held;

	if (database->Get(rocksdb::ReadOptions(), format_key, &held).ok())
	{
		if (held != format_version)
		{
			throw storage_error(
				"storage_error",
				ERROR("The store is format " + held + " and this build reads format " + format_version + "."));
		}

		return;
	}

	std::unique_ptr<rocksdb::Iterator> it(database->NewIterator(rocksdb::ReadOptions()));

	written(database->Put(rocksdb::WriteOptions(), format_key, format_version), "Writing the store format");
}

// Counts are reserved a block at a time so that stamping a write costs nothing, and the block is
// written down before any of it is handed out — so a process that ends without warning gives up
// the rest of its block rather than issuing counts the one after it will issue again.
void repository::rocksdb_repository::reserve_counts()
{
	std::string held;
	uint64_t reserved = 0;

	if (database->Get(rocksdb::ReadOptions(), count_key, &held).ok())
	{
		boost::conversion::try_lexical_convert(held, reserved);
	}

	written(
		database->Put(rocksdb::WriteOptions(), count_key, std::to_string(reserved + counts_per_reservation)),
		"Reserving a block of write counts");

	counts = reserved;
	counts_reserved = reserved + counts_per_reservation;
}

uint64_t repository::rocksdb_repository::next_count()
{
	std::lock_guard<std::mutex> lock(count_mutex);

	if (counts == counts_reserved)
	{
		reserve_counts();
	}

	return ++counts;
}

rocksdb::Options repository::rocksdb_repository::file_options() const
{
	return rocksdb::Options(rocksdb::DBOptions(), family_options);
}

std::string repository::rocksdb_repository::transfer_name() const
{
	return instance_name + "-" + std::to_string(transfers++) + ".sst";
}

repository::rocksdb_repository::~rocksdb_repository()
{
	close_handles();
}

// Every column family has to be closed before the database is, and the database outlives this
// because it is destroyed with the members afterwards.
void repository::rocksdb_repository::close_handles()
{
	for (std::map<std::string, rocksdb::ColumnFamilyHandle *>::iterator it = handles.begin(); it != handles.end(); ++it)
	{
		database->DestroyColumnFamilyHandle(it->second);
	}

	handles.clear();
}

void repository::rocksdb_repository::create_table(const table::table &table, const record::version &stamp)
{
	if (!table.is_valid)
	{
		return;
	}

	std::unique_lock<std::shared_mutex> lock(handle_mutex);

	open_family(table.name);
	tables.create(table, stamp);
	write_tables();
}

// The schema is parsed once and held, so these are memory reads behind the lock that a write to it
// takes exclusively. Every record request asks whether the table is there, which is what makes the
// cost of reading and parsing a record for each of them worth not paying.
std::set<table::table> repository::rocksdb_repository::list_tables() const
{
	std::shared_lock<std::shared_mutex> lock(handle_mutex);

	return tables.tables();
}

bool repository::rocksdb_repository::has_table(const std::string &table_name) const
{
	std::shared_lock<std::shared_mutex> lock(handle_mutex);

	return tables.has(table_name);
}

table::table repository::rocksdb_repository::read_table(const std::string &table_name) const
{
	std::shared_lock<std::shared_mutex> lock(handle_mutex);

	return tables.read(table_name);
}

void repository::rocksdb_repository::delete_table(const std::string &table_name, const record::version &stamp)
{
	std::unique_lock<std::shared_mutex> lock(handle_mutex);

	drop_family(table_name);

	// The tombstone is written whether or not this node had the table, because what it says is
	// that the name is gone as of this version — which is the answer a node that missed the
	// create needs as much as one that took it.
	tables.remove(table_name, stamp);
	write_tables();
}

table::schema repository::rocksdb_repository::read_schema() const
{
	std::shared_lock<std::shared_mutex> lock(handle_mutex);

	return tables;
}

size_t repository::rocksdb_repository::merge_schema(const table::schema &named)
{
	std::unique_lock<std::shared_mutex> lock(handle_mutex);
	std::vector<table::schema::change> changed = tables.merge(named);

	for (size_t i = 0; i < changed.size(); i++)
	{
		if (changed[i].live)
		{
			open_family(changed[i].name);
		}
		else
		{
			drop_family(changed[i].name);
		}
	}

	write_tables();

	return changed.size();
}

void repository::rocksdb_repository::open_family(const std::string &table_name)
{
	if (handles.find(table_name) != handles.end())
	{
		return;
	}

	rocksdb::ColumnFamilyHandle *handle = NULL;

	written(database->CreateColumnFamily(family_options, table_name, &handle), "Creating table \"" + table_name + "\"");

	handles.insert({ table_name, handle });
}

void repository::rocksdb_repository::drop_family(const std::string &table_name)
{
	std::map<std::string, rocksdb::ColumnFamilyHandle *>::iterator handle = handles.find(table_name);

	if (handle == handles.end())
	{
		return;
	}

	written(database->DropColumnFamily(handle->second), "Dropping table \"" + table_name + "\"");

	database->DestroyColumnFamilyHandle(handle->second);
	handles.erase(handle);
}

void repository::rocksdb_repository::read_tables()
{
	std::string held;

	if (database->Get(rocksdb::ReadOptions(), schema_key, &held).ok())
	{
		tables = table::to_schema(held);
	}

	// A column family the schema does not name is a table whose two writes — the family and the
	// record naming it — did not both land. The record is what the cluster agrees on, so it is
	// what the families are made to match.
	std::set<std::string> named = tables.names();
	std::vector<std::string> orphaned;

	for (std::map<std::string, rocksdb::ColumnFamilyHandle *>::const_iterator it = handles.begin(); it != handles.end();
		 ++it)
	{
		if (it->first != rocksdb::kDefaultColumnFamilyName && named.find(it->first) == named.end())
		{
			orphaned.push_back(it->first);
		}
	}

	for (size_t i = 0; i < orphaned.size(); i++)
	{
		drop_family(orphaned[i]);
	}

	for (std::set<std::string>::const_iterator it = named.begin(); it != named.end(); ++it)
	{
		open_family(*it);
	}
}

// The document carries a version for each name in it, so the record itself needs none: a schema
// version would be one number vouching for operations this node may never have applied.
void repository::rocksdb_repository::write_tables()
{
	written(
		database->Put(rocksdb::WriteOptions(), schema_key, boost::json::serialize(tables.json())),
		"Writing the schema");
}

void repository::rocksdb_repository::write_record(const std::string &table_name, const record::record &record)
{
	std::shared_lock<std::shared_mutex> lock(handle_mutex);

	written(
		database->Put(
			rocksdb::WriteOptions(),
			table_handle(table_name),
			record.key,
			record::compose_value(record.stamp, record.value)),
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

	return std::string(record::value_of(value));
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
		record::record read = record::valid_record(
			it->key().ToString(),
			range.values ? std::string(record::value_of(it->value().ToStringView())) : "");

		read.stamp = range.values ? record::version_of(it->value().ToStringView()) : record::version();

		page.records.push_back(read);
	}

	check(it->status(), "Scanning \"" + table_name + "\"");

	return page;
}

// The keys the store's own files start at, weighted by how much is in each. A level compaction
// leaves the levels overlapping, so this is a sample of the key space and not a partition of it —
// which is all a split has to be.
std::vector<std::string> repository::rocksdb_repository::split_points(const std::string &table_name, size_t ways) const
{
	std::vector<std::string> points;

	if (ways < 2)
	{
		return points;
	}

	std::shared_lock<std::shared_mutex> lock(handle_mutex);
	rocksdb::ColumnFamilyMetaData metadata;

	database->GetColumnFamilyMetaData(table_handle(table_name), &metadata);

	std::vector<std::pair<std::string, uint64_t>> files;
	uint64_t held = 0;

	for (size_t level = 0; level < metadata.levels.size(); level++)
	{
		const std::vector<rocksdb::SstFileMetaData> &at = metadata.levels[level].files;

		for (size_t i = 0; i < at.size(); i++)
		{
			// The first key of a file is a key some record is at or after, which is what a bound
			// has to be. Its size is what says how much of the table is behind it.
			files.push_back(std::pair<std::string, uint64_t>(at[i].smallestkey, at[i].size));

			held += at[i].size;
		}
	}

	std::sort(files.begin(), files.end());

	uint64_t taken = 0;
	size_t next = 1;

	for (size_t i = 0; i < files.size() && next < ways; i++)
	{
		taken += files[i].second;

		// The share of the table this key is past, against the share the next split point wants.
		if (taken * ways >= held * next && !files[i].first.empty())
		{
			// A key that is already a split point is a piece with nothing in it, which is a worker
			// with nothing to do rather than a piece somebody else loses.
			if (points.empty() || points.back() != files[i].first)
			{
				points.push_back(files[i].first);
			}

			next++;
		}
	}

	// The last piece runs to the end of the table, so the key that would have started it is not a
	// bound anybody needs.
	if (!points.empty() && points.size() == ways)
	{
		points.pop_back();
	}

	return points;
}

repository::extract repository::rocksdb_repository::export_records(const std::string &table_name, const share &wanted)
	const
{
	std::shared_lock<std::shared_mutex> lock(handle_mutex);
	std::string what = "Exporting a file of \"" + table_name + "\"";
	rocksdb::ReadOptions options;
	rocksdb::Slice lower(wanted.from);
	rocksdb::Slice upper(wanted.to);

	if (wanted.has_from)
	{
		options.iterate_lower_bound = &lower;
	}

	std::unique_ptr<rocksdb::Iterator> it(database->NewIterator(options, table_handle(table_name)));
	scratch_file written_file(transfer_directory, transfer_name());
	rocksdb::SstFileWriter writer(rocksdb::EnvOptions(), file_options());
	extract taken;
	size_t bytes = 0;

	check(writer.Open(written_file.path()), what);

	for (it->SeekToFirst(); it->Valid(); it->Next())
	{
		// A bound is inclusive, so every file after the first begins again at the key it resumed
		// at.
		if (wanted.has_from && it->key().compare(lower) == 0)
		{
			continue;
		}

		// And the end of the piece is inclusive, so that the key one worker stops at is the key
		// the next one starts after.
		if (wanted.has_to && it->key().compare(upper) > 0)
		{
			break;
		}

		std::string key = it->key().ToString();

		if (wanted.partitions.test(cluster::partition_of(key)))
		{
			// A walk that is not carrying values carries the versions, which is the whole of what
			// a node deciding whether to give a record up has to ask about.
			rocksdb::Slice stored = it->value();
			rocksdb::Slice carried =
				wanted.values ? stored : rocksdb::Slice(stored.data(), std::min(stored.size(), record::version_size));

			check(writer.Put(it->key(), carried), what);

			taken.records++;
		}

		// The budget is what the walk read and not what it wrote, so a node holding a share of a
		// zone reads its way through the table once over the whole transfer rather than once for
		// every file of it. A walk that is not carrying values has not read them either.
		bytes += it->key().size() + (wanted.values ? it->value().size() : 0);

		if (bytes >= wanted.bytes)
		{
			taken.last = key;

			it->Next();

			taken.has_more = it->Valid();

			break;
		}
	}

	check(it->status(), what);

	// A file with nothing in it is one SstFileWriter refuses to finish, and one there would be
	// nothing to send. Where the walk reached still stands: the file after this one resumes there.
	if (taken.records == 0)
	{
		return taken;
	}

	check(writer.Finish(), what);

	taken.file = written_file.read();

	return taken;
}

size_t repository::rocksdb_repository::import_records(const std::string &table_name, const std::string &file)
{
	if (file.empty())
	{
		return 0;
	}

	std::shared_lock<std::shared_mutex> lock(handle_mutex);
	std::string what = "Importing a file into \"" + table_name + "\"";
	rocksdb::ColumnFamilyHandle *handle = table_handle(table_name);
	scratch_file given(transfer_directory, transfer_name());

	if (!given.write(file))
	{
		throw storage_error("storage_error", what + " failed: the file could not be written.");
	}

	rocksdb::SstFileReader reader(file_options());

	check(reader.Open(given.path()), what);

	size_t records = 0;
	size_t held = 0;

	// Two walks of the file rather than one, because the first is what says whether the second is
	// needed: a store holding none of these keys — a node being rebuilt, which is most of them —
	// takes the file it was sent and never writes a second copy of it.
	{
		std::unique_ptr<rocksdb::Iterator> it(reader.NewIterator(rocksdb::ReadOptions()));
		rocksdb::PinnableSlice value;

		for (it->SeekToFirst(); it->Valid(); it->Next())
		{
			value.Reset();

			if (!database->Get(rocksdb::ReadOptions(), handle, it->key(), &value).ok())
			{
				records++;

				continue;
			}

			held++;

			if (record::is_newer(it->value().ToStringView(), value.ToStringView()))
			{
				records++;
			}
		}

		check(it->status(), what);
	}

	if (records == 0)
	{
		return 0;
	}

	rocksdb::IngestExternalFileOptions ingest;

	// The file is on the store's own volume, so it is linked into place rather than copied.
	ingest.move_files = true;
	ingest.snapshot_consistency = false;

	if (held == 0)
	{
		written(database->IngestExternalFile(handle, { given.path() }, ingest), what);

		return records;
	}

	scratch_file kept(transfer_directory, transfer_name());
	rocksdb::SstFileWriter writer(rocksdb::EnvOptions(), file_options());
	std::unique_ptr<rocksdb::Iterator> it(reader.NewIterator(rocksdb::ReadOptions()));
	rocksdb::PinnableSlice value;

	check(writer.Open(kept.path()), what);

	records = 0;

	for (it->SeekToFirst(); it->Valid(); it->Next())
	{
		value.Reset();

		bool absent = !database->Get(rocksdb::ReadOptions(), handle, it->key(), &value).ok();

		// An ingested file is written above what is already there, so a key it carries at a later
		// version replaces the one held and a key it carries at an earlier one is left out of it.
		if (absent || record::is_newer(it->value().ToStringView(), value.ToStringView()))
		{
			check(writer.Put(it->key(), it->value()), what);

			records++;
		}
	}

	check(it->status(), what);

	// A write that landed between the two walks is a key this store now holds, and the second walk
	// is the one that decides: there may be nothing left to take.
	if (records == 0)
	{
		return 0;
	}

	check(writer.Finish(), what);

	written(database->IngestExternalFile(handle, { kept.path() }, ingest), what);

	return records;
}

size_t repository::rocksdb_repository::clear_records(const std::string &table_name, const std::string &file)
{
	if (file.empty())
	{
		return 0;
	}

	std::shared_lock<std::shared_mutex> lock(handle_mutex);
	std::string what = "Clearing records of \"" + table_name + "\"";
	rocksdb::ColumnFamilyHandle *handle = table_handle(table_name);
	scratch_file given(transfer_directory, transfer_name());

	if (!given.write(file))
	{
		throw storage_error("storage_error", what + " failed: the file could not be written.");
	}

	rocksdb::SstFileReader reader(file_options());

	check(reader.Open(given.path()), what);

	std::unique_ptr<rocksdb::Iterator> it(reader.NewIterator(rocksdb::ReadOptions()));
	rocksdb::WriteBatch batch;
	rocksdb::PinnableSlice value;
	size_t cleared = 0;

	for (it->SeekToFirst(); it->Valid(); it->Next())
	{
		value.Reset();

		// Most of what the file carries was never here: it is everything the node that owns these
		// partitions holds, and what this node is giving up is the fraction that just moved.
		if (!database->Get(rocksdb::ReadOptions(), handle, it->key(), &value).ok())
		{
			continue;
		}

		// What is here was written after what the owner holds, so the owner is a copy behind
		// rather than a copy to hand over to. The pass that fetches is what settles it.
		if (record::is_newer(value.ToStringView(), it->value().ToStringView()))
		{
			continue;
		}

		batch.Delete(handle, it->key());

		cleared++;
	}

	check(it->status(), what);

	if (cleared > 0)
	{
		written(database->Write(rocksdb::WriteOptions(), &batch), what);
	}

	return cleared;
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
