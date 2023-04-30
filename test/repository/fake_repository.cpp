#include <algorithm>

#include "fake_repository.h"

repository::fake_repository::fake_repository()
{
}

void repository::fake_repository::create_table(const table::table &table)
{
	if (table.is_valid())
	{
		tables.insert(std::pair(table.get_name(), boost::json::serialize(table.to_json())));
	}
}

std::set<table::valid_table> repository::fake_repository::list_tables()
{
	std::set<table::valid_table> table_list;

	for (std::map<std::string, std::string>::iterator it = tables.begin(); it != tables.end(); ++it)
	{
		table_list.insert(table::valid_table(it->second));
	}

	return table_list;
}

bool repository::fake_repository::has_table(const table::table &table)
{
	return table.is_valid() && tables.count(table.get_name()) > 0;
}
