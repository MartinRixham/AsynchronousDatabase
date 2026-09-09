#ifndef REPOSITORY_FAKE_REPOSITORY_H
#define REPOSITORY_FAKE_REPOSITORY_H

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "repository/repository.h"

namespace repository
{
	class fake_repository : public repository
	{
		std::map<std::string, std::string> tables;

		// A table of its own per table, ordered by the byte comparison std::string already makes,
		// which is the ordering RocksDB gives a column family.
		std::map<std::string, std::map<std::string, std::string>> records;

		bool stalled = false;

		// A walk hands the files of a share to several threads at once, so a store this stands in
		// for is one that is written from several at once. Held by pointer because the fixtures
		// that build one of these hand it back by value.
		std::shared_ptr<std::mutex> mutex;

	public:
		fake_repository();

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

		void stall();

	private:
		// Whether the table is there, for a caller that is holding the lock already.
		bool holds(const std::string &table_name) const;
	};
}

#endif
