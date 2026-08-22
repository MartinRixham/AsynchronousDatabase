#include <algorithm>
#include <vector>

#include "fake_repository.h"

namespace
{
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

	for (size_t i = 0; i < keys.size(); i++)
	{
		if (page.records.size() == range.limit)
		{
			page.has_more = true;
			break;
		}

		page.records.push_back(
			record::valid_record(keys[i], range.values ? table_records.at(keys[i]) : ""));
	}

	return page;
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
