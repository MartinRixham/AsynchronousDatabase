#include "table.h"

bool table::operator<(const table &lhs, const table &rhs)
{
	return lhs.name < rhs.name;
}

bool table::operator==(const table &lhs, const table &rhs)
{
	return lhs.is_valid == rhs.is_valid && lhs.json == rhs.json;
}

table::table table::parse_table(
	const std::string &name,
	const boost::json::object &json,
	const std::set<std::string> &tables)
{
	if (!is_valid_name(name))
	{
		return invalid_table(
			"invalid_table_name",
			"Table name \"" +
				name +
				"\" is not 1 to " +
				std::to_string(max_name_size) +
				" characters of [A-Za-z0-9_ -], or is reserved.");
	}

	if (json.contains("immutable") && !json.at("immutable").is_bool())
	{
		return invalid_table("invalid_body", "The immutable option of a table is true or false.");
	}

	bool immutable = json.contains("immutable") && json.at("immutable").as_bool();

	// A dependency is a name and nothing else: the API records the edge, it does not run the work.
	if (!json.contains("dependencies"))
	{
		return valid_table(name, std::vector<std::string>(), immutable);
	}

	if (!json.at("dependencies").is_array())
	{
		return invalid_table("dependency_not_found", "Dependencies are not a list of table names.");
	}

	const boost::json::array dependency_array = json.at("dependencies").as_array();
	std::vector<std::string> dependencies;

	for (size_t i = 0; i < dependency_array.size(); i++)
	{
		if (!dependency_array[i].is_string())
		{
			return invalid_table("dependency_not_found", "A dependency is not the name of a table.");
		}

		std::string dependency = std::string(dependency_array[i].as_string());

		if (tables.find(dependency) == tables.end())
		{
			return invalid_table("dependency_not_found", "Dependency \"" + dependency + "\" is not a table.");
		}

		dependencies.push_back(dependency);
	}

	return valid_table(name, dependencies, immutable);
}

table::table table::valid_table(
	const std::string &name,
	const std::vector<std::string> &dependencies,
	bool immutable)
{
	boost::json::array dependency_array;

	for (size_t i = 0; i < dependencies.size(); i++)
	{
		dependency_array.push_back(boost::json::string(dependencies[i]));
	}

	boost::json::object json {
		{ "name", boost::json::string(name) },
		{ "dependencies", dependency_array },
		{ "immutable", immutable } };

	return { true, name, json, immutable, "", "" };
}

table::table table::invalid_table(const std::string &code, const std::string &message)
{
	boost::json::object error { { "code", code }, { "message", message } };
	boost::json::object json { { "error", error } };

	return { false, "", json, false, code, message };
}

table::table table::to_table(const std::string &json)
{
	boost::json::object table_object = boost::json::parse(json).as_object();
	std::string name = std::string(table_object["name"].as_string());
	bool immutable = table_object.contains("immutable") && table_object.at("immutable").as_bool();

	table_object["immutable"] = immutable;

	return { true, name, table_object, immutable, "", "" };
}

bool table::is_valid_name(const std::string &name)
{
	if (name.empty() || name.size() > max_name_size || name == "default")
	{
		return false;
	}

	for (size_t i = 0; i < name.size(); i++)
	{
		char character = name[i];

		if (!(character >= 'a' && character <= 'z') &&
			!(character >= 'A' && character <= 'Z') &&
			!(character >= '0' && character <= '9') &&
			character != ' ' &&
			character != '_' &&
			character != '-')
		{
			return false;
		}
	}

	return true;
}
