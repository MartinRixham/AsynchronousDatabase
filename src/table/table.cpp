#include "valid_table.h"
#include "invalid_table.h"

table::table *table::parse_table(boost::json::object json, std::set<std::string> tables)
{
	if (!json.contains("name") || json["name"].as_string().size() < 1)
	{
		return new invalid_table("Table requires name of length greater than 0.");
	}

	std::string name = std::string(json["name"].as_string()); 
	
	if (tables.find(name) != tables.end())
	{
		return new invalid_table("A table with the name \"" + name + "\" already exists.");
	}

	boost::json::array dependency_array = json["dependencies"].as_array();
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
			return new invalid_table("Dependency \"" + dependency + "\" is not a table.");
		}
	}

	return new valid_table(name, dependencies);
}
