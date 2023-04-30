#include "valid_table.h"

table::valid_table::valid_table(const std::string &name, const std::vector<std::string> &dependencies):
	name(name),
	dependencies(dependencies)
{
}

table::valid_table::valid_table(const std::string &json)
{
	boost::json::object table_object = boost::json::parse(json).as_object();

	name = table_object["name"].as_string();

	boost::json::array dependency_array = table_object["dependencies"].as_array();

	for (boost::json::array::size_type i = 0; i < dependency_array.size(); i++)
	{
		dependencies.push_back(std::string(dependency_array[i].as_string()));
	}
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

bool table::valid_table::operator < (const valid_table &other) const
{
	return name < other.name;
}
