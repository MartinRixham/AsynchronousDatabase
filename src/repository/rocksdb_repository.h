#ifndef REPOSITORY_ROCSKDB_REPOSITORY_H
#define REPOSITORY_ROCSKDB_REPOSITORY_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include "repository.h"

namespace repository
{
	// What the store may keep in memory: the block cache and the memtables together, from
	// ASYNCDB_MEMORY. It is one number because it is one thing an operator sizes to the instance,
	// and the default is the smallest machine this is meant to run on rather than the largest.
	constexpr size_t default_memory_bytes = 512 * 1024 * 1024;

	class rocksdb_repository : public repository
	{
		// Declared before the database so that what the database holds a share of outlives it.
		std::shared_ptr<rocksdb::Cache> block_cache;

		// What every column family is opened and created with. A table is a column family, so a
		// table created long after the store was opened is given the same options as the rest.
		rocksdb::ColumnFamilyOptions family_options;

		std::unique_ptr<rocksdb::DB> database;

		std::map<std::string, rocksdb::ColumnFamilyHandle *> handles;

		mutable std::shared_mutex handle_mutex;

		std::string instance_name;

		// Where a transfer's files are built, which is a directory of the store's own so that they
		// land on the volume the store is on rather than wherever a temporary file would go.
		std::filesystem::path transfer_directory;

		// What names them apart. Several exports can be in flight at once, and each is a file.
		mutable std::atomic<uint64_t> transfers = 0;

	public:
		explicit rocksdb_repository(const std::string &directory, size_t memory_bytes = default_memory_bytes);

		~rocksdb_repository();

		rocksdb_repository(const rocksdb_repository &) = delete;

		rocksdb_repository &operator=(const rocksdb_repository &) = delete;

		void create_table(const table::table &table) override;

		std::set<table::table> list_tables() const override;

		bool has_table(const std::string &table_name) const override;

		table::table read_table(const std::string &table_name) const override;

		void delete_table(const std::string &table_name) override;

		void write_record(const std::string &table_name, const record::record &record) override;

		std::optional<std::string> read_record(const std::string &table_name, const std::string &key) const override;

		void delete_record(const std::string &table_name, const std::string &key) override;

		scan::page scan_records(const std::string &table_name, const scan::range &range) const override;

		std::vector<std::string> split_points(const std::string &table_name, size_t ways) const override;

		extract export_records(const std::string &table_name, const share &wanted) const override;

		size_t import_records(const std::string &table_name, const std::string &file) override;

		size_t clear_records(const std::string &table_name, const std::string &file) override;

		void delete_records(const std::string &table_name, const scan::range &range) override;

		bool is_write_stalled() const override;

		std::string instance() const override;

	private:
		void written(const rocksdb::Status &status, const std::string &what);

		rocksdb::ColumnFamilyHandle *table_handle(const std::string &table_name) const;

		// The options a transfer's files are written and read with, which are the store's own: a
		// file is written here and read by the node it is sent to, and both of them are this code.
		rocksdb::Options file_options() const;

		std::string transfer_name() const;
	};
}

#endif
