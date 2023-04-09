#include <algorithm>

#include "fake_repository.h"

repository::fake_repository::fake_repository()
{
}

void repository::fake_repository::create_table(const std::string &name)
{
	tables.push_back(name);
}

std::vector<std::string> repository::fake_repository::list_tables()
{
	std::vector<std::string> copy;

	copy.assign(tables.begin(), tables.end());

	return copy;
}

bool repository::fake_repository::has_table(const std::string &name)
{
	return std::count(tables.begin(), tables.end(), name) > 0;
}
