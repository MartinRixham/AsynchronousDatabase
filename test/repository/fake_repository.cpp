#include <algorithm>

#include "fake_repository.h"

repository::fake_repository::fake_repository()
{
}

void repository::fake_repository::create_table(const std::string &name)
{
	tables.insert(std::pair(name, "{ \"name\": \"" + name +"\" }"));
}

std::vector<std::string> repository::fake_repository::list_tables()
{
	std::vector<std::string> table_list;

	for (std::map<std::string, std::string>::iterator it = tables.begin(); it != tables.end(); ++it)
	{
		table_list.push_back(it->second);
	}

	return table_list;
}

bool repository::fake_repository::has_table(const std::string &name)
{
	return tables.count(name) > 0;
}
