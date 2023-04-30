#include "table.h"

bool table::operator <(const table &lhs, const table &rhs)
{
	return lhs.name < rhs.name;
}

table::table table::parse_table(const boost::json::object &json, const std::set<std::string> &tables)
{
	if (!json.contains("name") || json.at("name").as_string().size() < 1)
	{
		return invalid_table("Table requires name of length greater than 0.");
	}

	std::string name = std::string(json.at("name").as_string()); 
	
	if (tables.find(name) != tables.end())
	{
		return invalid_table("A table with the name \"" + name + "\" already exists.");
	}

	boost::json::array dependency_array = json.at("dependencies").as_array();
	std::vector<std::string> dependencies;

	for (size_t i = 0; i < dependency_array.size(); i++)
	{
		std::string dependency = std::string(dependency_array[i].as_string());

		if (tables.find(dependency) != tables.end())
		{
			dependencies.push_back(dependency);
		}
		else
		{
			return invalid_table("Dependency \"" + dependency + "\" is not a table.");
		}
	}

	return valid_table(name, dependencies);
}

table::table table::valid_table(const std::string &name, const std::vector<std::string> &dependencies)
{
	boost::json::object json;

	json.insert(std::pair("name", boost::json::string(name)));

	boost::json::array dependency_array;

	for (size_t i = 0; i < dependencies.size(); i++)
	{
		dependency_array.push_back(boost::json::string(dependencies[i]));
	}

	json.insert(std::pair("dependencies", dependency_array));

	return { true, name, json };
}

table::table table::invalid_table(const std::string &error)
{
	boost::json::object json;

	json.insert(std::pair<std::string, boost::json::string>("error", error));

	return { false, "", json };
}

table::table table::to_table(const std::string &json)
{
	boost::json::object table_object = boost::json::parse(json).as_object();
	std::string name = std::string(table_object["name"].as_string());

	return { true, name, table_object };
}
