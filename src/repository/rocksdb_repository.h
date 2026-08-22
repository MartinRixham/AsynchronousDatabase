#ifndef REPOSITORY_ROCSKDB_REPOSITORY_H
#define REPOSITORY_ROCSKDB_REPOSITORY_H

#include <map>
#include <memory>
#include <shared_mutex>
#include <string>

#include <rocksdb/db.h>

#include "repository.h"

namespace repository
{
	class rocksdb_repository : public repository
	{
		std::unique_ptr<rocksdb::DB> database;

		// A table is a column family: dropping one takes its data with it, its options are its
		// own, and the boundary between two tables is structural rather than a prefix.
		std::map<std::string, rocksdb::ColumnFamilyHandle *> handles;

		mutable std::shared_mutex handle_mutex;

		std::string instance_name;

	public:
		explicit rocksdb_repository(const std::string &directory);

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

		void delete_records(const std::string &table_name, const scan::range &range) override;

		bool is_write_stalled() const override;

		std::string instance() const override;

	private:
		rocksdb::ColumnFamilyHandle *table_handle(const std::string &table_name) const;
	};
}

#endif
