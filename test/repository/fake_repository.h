#ifndef REPOSITORY_FAKE_REPOSITORY_H
#define REPOSITORY_FAKE_REPOSITORY_H

#include <map>
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

		void delete_records(const std::string &table_name, const scan::range &range) override;

		bool is_write_stalled() const override;

		std::string instance() const override;

		void stall();
	};
}

#endif
