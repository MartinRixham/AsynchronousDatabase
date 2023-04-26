#include "table.h"

table::table::table(const std::string &name, const std::vector<std::string> &dependencies):
	name(name),
	dependencies(dependencies)
{
}

std::string table::table::getName() const
{
	return name;
}

boost::json::object table::table::toJson() const
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

table::table table::parseTable(boost::json::object json)
{
	boost::json::array dependency_array = json["dependencies"].as_array();
	std::vector<std::string> dependencies;

	for (size_t i = 0; i < dependency_array.size(); i++)
	{
		dependencies.push_back(std::string(dependency_array[i].as_string()));
	}

	return table(std::string(json["name"].as_string()), dependencies);
}
