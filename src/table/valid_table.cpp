#include "valid_table.h"

table::valid_table::valid_table(const std::string &name, const std::vector<std::string> &dependencies):
	name(name),
	dependencies(dependencies)
{
}

bool table::valid_table::is_valid() const
{
	return true;
}

std::string table::valid_table::get_name() const
{
	return name;
}

boost::json::object table::valid_table::to_json() const
{
	boost::json::object json;

	json.insert(std::pair("name", boost::json::string(name)));

	boost::json::array dependency_array;

	for (size_t i = 0; i < dependencies.size(); i++)
	{
		dependency_array.push_back(boost::json::string(dependencies[i]));
	}

	json.insert(std::pair("dependencies", dependency_array));

	return json;
}
