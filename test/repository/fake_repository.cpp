#include <algorithm>

#include "fake_repository.h"

repository::fake_repository::fake_repository()
{
}

void repository::fake_repository::create_table(const table::table &table)
{
	if (table.is_valid)
	{
		tables.insert({ table.name, boost::json::serialize(table.json) });
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

bool repository::fake_repository::has_table(const table::table &table) const
{
	return table.is_valid && tables.count(table.name) > 0;
}
