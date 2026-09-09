#include <algorithm>
#include <optional>
#include <vector>

#include <boost/lexical_cast/try_lexical_convert.hpp>

#include "fake_repository.h"

namespace
{
	void append(std::string &file, const std::string &text)
	{
		file += std::to_string(text.size());
		file += '\n';
		file += text;
	}

	// One length prefixed string, leaving the position after it. Nothing when the file does not
	// carry one there, which ends the read rather than guessing at the rest.
	std::optional<std::string> take(const std::string &file, size_t &position)
	{
		size_t newline = file.find('\n', position);

		if (newline == std::string::npos)
		{
			return std::nullopt;
		}

		size_t length = 0;

		if (!boost::conversion::try_lexical_convert(file.substr(position, newline - position), length) ||
			newline + 1 + length > file.size())
		{
			return std::nullopt;
		}

		position = newline + 1 + length;

		return file.substr(newline + 1, length);
	}

	bool is_in_range(const std::string &key, const scan::range &range)
	{
		return (!range.has_from || key >= range.from) && (!range.has_to || key < range.to);
	}
}

repository::fake_repository::fake_repository()
{
}

void repository::fake_repository::create_table(const table::table &table)
{
	if (table.is_valid)
	{
		tables[table.name] = boost::json::serialize(table.json);
		records[table.name];
	}
}

std::set<table::table> repository::fake_repository::list_tables() const
{
	std::set<table::table> table_list;

	for (std::map<std::string, std::string>::const_iterator it = tables.begin(); it != tables.end(); ++it)
	{
		table_list.insert(table::to_table(it->second));
	}

	return table_list;
}

bool repository::fake_repository::has_table(const std::string &table_name) const
{
	return tables.count(table_name) > 0;
}

table::table repository::fake_repository::read_table(const std::string &table_name) const
{
	if (tables.count(table_name))
	{
		return table::to_table(tables.at(table_name));
	}

	return table::invalid_table("table_not_found", "No table named \"" + table_name + "\".");
}

void repository::fake_repository::delete_table(const std::string &table_name)
{
	// The data goes with the table, as it goes with a dropped column family.
	tables.erase(table_name);
	records.erase(table_name);
}

void repository::fake_repository::write_record(const std::string &table_name, const record::record &record)
{
	if (!has_table(table_name))
	{
		throw storage_error("table_not_found", "No table named \"" + table_name + "\".");
	}

	records[table_name][record.key] = record.value;
}

std::optional<std::string> repository::fake_repository::read_record(
	const std::string &table_name,
	const std::string &key) const
{
	if (!has_table(table_name) || records.at(table_name).count(key) == 0)
	{
		return std::nullopt;
	}

	return records.at(table_name).at(key);
}

void repository::fake_repository::delete_record(const std::string &table_name, const std::string &key)
{
	if (has_table(table_name))
	{
		records[table_name].erase(key);
	}
}

scan::page repository::fake_repository::scan_records(const std::string &table_name, const scan::range &range) const
{
	scan::page page;

	if (!has_table(table_name))
	{
		return page;
	}

	const std::map<std::string, std::string> &table_records = records.at(table_name);
	std::vector<std::string> keys;

	for (std::map<std::string, std::string>::const_iterator it = table_records.begin();
		it != table_records.end();
		++it)
	{
		if (is_in_range(it->first, range))
		{
			keys.push_back(it->first);
		}
	}

	if (range.reverse)
	{
		std::reverse(keys.begin(), keys.end());
	}

	size_t bytes = 0;

	for (size_t i = 0; i < keys.size(); i++)
	{
		if (page.records.size() == range.limit)
		{
			page.has_more = true;
			break;
		}

		// The byte budget of scan::max_page_bytes, ended the same way the real store ends it, so
		// that a test against this one sees the page a client would really be given.
		size_t size = keys[i].size() + (range.values ? table_records.at(keys[i]).size() : 0);

		if (!page.records.empty() && bytes + size > scan::max_page_bytes)
		{
			page.has_more = true;
			break;
		}

		bytes += size;

		page.records.push_back(
			record::valid_record(keys[i], range.values ? table_records.at(keys[i]) : ""));
	}

	return page;
}

// A file of this store is its records written out one after another, each behind its length. It is
// not an SST and does not need to be: what a file carries is settled by the seam, and both ends of
// a transfer between two of these are this code.
repository::extract repository::fake_repository::export_records(
	const std::string &table_name,
	const share &wanted) const
{
	extract taken;

	if (!has_table(table_name))
	{
		return taken;
	}

	const std::map<std::string, std::string> &table_records = records.at(table_name);
	size_t bytes = 0;

	for (std::map<std::string, std::string>::const_iterator it = table_records.begin();
		it != table_records.end();
		++it)
	{
		// A bound is inclusive, so every file after the first begins again at the key it resumed
		// at.
		if (wanted.has_from && it->first <= wanted.from)
		{
			continue;
		}

		if (wanted.partitions.test(cluster::partition_of(it->first)))
		{
			append(taken.file, it->first);
			append(taken.file, it->second);

			taken.records++;
		}

		// The budget of the real store, ended the way the real store ends it: what the walk read
		// and not what it wrote.
		bytes += it->first.size() + it->second.size();

		if (bytes >= wanted.bytes)
		{
			taken.last = it->first;

			std::map<std::string, std::string>::const_iterator next = it;

			taken.has_more = ++next != table_records.end();

			break;
		}
	}

	if (taken.records == 0)
	{
		taken.file.clear();
	}

	return taken;
}

size_t repository::fake_repository::import_records(const std::string &table_name, const std::string &file)
{
	if (!has_table(table_name))
	{
		return 0;
	}

	std::map<std::string, std::string> &table_records = records[table_name];
	size_t position = 0;
	size_t taken = 0;

	while (position < file.size())
	{
		std::optional<std::string> key = take(file, position);
		std::optional<std::string> value = take(file, position);

		if (!key || !value)
		{
			return taken;
		}

		// A key this store already holds is kept, which is what the real store does and what makes
		// a fetch safe: what is here was written after the ownership moved and what is in the file
		// was written before it.
		if (table_records.try_emplace(*key, *value).second)
		{
			taken++;
		}
	}

	return taken;
}

void repository::fake_repository::delete_records(const std::string &table_name, const scan::range &range)
{
	if (!has_table(table_name))
	{
		return;
	}

	std::map<std::string, std::string> &table_records = records[table_name];
	std::map<std::string, std::string>::iterator it = table_records.begin();

	while (it != table_records.end())
	{
		if (is_in_range(it->first, range))
		{
			it = table_records.erase(it);
		}
		else
		{
			++it;
		}
	}
}

bool repository::fake_repository::is_write_stalled() const
{
	return stalled;
}

std::string repository::fake_repository::instance() const
{
	return "fake";
}

void repository::fake_repository::stall()
{
	stalled = true;
}
