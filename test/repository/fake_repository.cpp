#include <string>

#include "fake_repository.h"

repository::fake_repository::fake_repository()
{
}

void repository::fake_repository::create_table(const std::string &name)
{
	tables.insert(name);
}

bool repository::fake_repository::has_table(const std::string &name)
{
	return tables.count(name) > 0;
}
