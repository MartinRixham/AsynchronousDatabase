#include "valid_table.h"
#include "invalid_table.h"

table::table *table::parse_table(boost::json::object json)
{
	if (!json.contains("name") || json["name"].as_string().size() < 1)
	{
		return new invalid_table("Table requires name of length greater than 0.");
	}

	boost::json::array dependency_array = json["dependencies"].as_array();
	std::vector<std::string> dependencies;

	for (size_t i = 0; i < dependency_array.size(); i++)
	{
		dependencies.push_back(std::string(dependency_array[i].as_string()));
	}

	return new valid_table(std::string(json["name"].as_string()), dependencies);
}
