#ifndef REPOSITORY_REPOSITORY_H
#define REPOSITORY_REPOSITORY_H

#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#include "record/record.h"
#include "scan/scan.h"
#include "table/table.h"

namespace repository
{
	// Genuine infrastructure failure, as opposed to a validation failure, which is a value. The
	// code is one of the documented error codes, so that back pressure is told from a real error.
	class storage_error : public std::runtime_error
	{
		std::string error_code;

	public:
		storage_error(const std::string &code, const std::string &message);

		const std::string &code() const;
	};

	class repository
	{
	public:
		virtual void create_table(const table::table &table) = 0;

		virtual std::set<table::table> list_tables() const = 0;

		virtual bool has_table(const std::string &table_name) const = 0;

		virtual table::table read_table(const std::string &table_name) const = 0;

		virtual void delete_table(const std::string &table_name) = 0;

		virtual void write_record(const std::string &table_name, const record::record &record) = 0;

		virtual std::optional<std::string> read_record(const std::string &table_name, const std::string &key) const = 0;

		virtual void delete_record(const std::string &table_name, const std::string &key) = 0;

		virtual scan::page scan_records(const std::string &table_name, const scan::range &range) const = 0;

		virtual void delete_records(const std::string &table_name, const scan::range &range) = 0;

		virtual bool is_write_stalled() const = 0;

		// Names the iteration a cursor belongs to, so that a cursor this instance did not issue
		// is refused rather than resumed against a different set of keys.
		virtual std::string instance() const = 0;
	};
}

#endif
