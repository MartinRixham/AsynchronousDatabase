#include <algorithm>
#include <expected>
#include <iterator>

#include "error/error_message.h"

#include "table.h"

table::table::table(const std::string &name, const std::vector<std::string> &dependencies):
	table_name(name),
	dependency_names(dependencies)
{
}

const std::string &table::table::name() const
{
	return table_name;
}

bool table::operator<(const table &lhs, const table &rhs)
{
	return lhs.name() < rhs.name();
}

std::expected<table::table, error::error_message> table::parse_table(
	const std::string &name,
	const boost::json::object &json,
	const std::set<std::string> &tables)
{
	if (!is_valid_name(name))
	{
		return std::unexpected(
			error::error_message { error::code::invalid_table_name,
								   "Table name \"" +
									   name +
									   "\" is not 1 to " +
									   std::to_string(max_name_size) +
									   " characters of [A-Za-z0-9_ -], or is reserved." });
	}

	// A dependency is a name and nothing else: the API records the edge, it does not run the work.
	if (!json.contains("dependencies"))
	{
		return table(name, std::vector<std::string>());
	}

	if (!json.at("dependencies").is_array())
	{
		return std::unexpected(
			error::error_message { error::code::dependency_not_found, "Dependencies are not a list of table names." });
	}

	const boost::json::array dependency_array = json.at("dependencies").as_array();
	std::vector<std::string> dependencies;

	for (const auto &element : dependency_array)
	{
		if (!element.is_string())
		{
			return std::unexpected(
				error::error_message { error::code::dependency_not_found, "A dependency is not the name of a table." });
		}

		std::string dependency = std::string(element.as_string());

		if (tables.find(dependency) == tables.end())
		{
			return std::unexpected(
				error::error_message { error::code::dependency_not_found,
									   "Dependency \"" + dependency + "\" is not a table." });
		}

		dependencies.push_back(dependency);
	}

	return table(name, dependencies);
}

table::table table::to_table(const boost::json::object &json)
{
	std::string name;
	std::vector<std::string> dependencies;

	if (json.contains("name") && json.at("name").is_string())
	{
		name = std::string(json.at("name").as_string());
	}

	if (json.contains("dependencies") && json.at("dependencies").is_array())
	{
		for (const auto &dependency : json.at("dependencies").as_array())
		{
			if (dependency.is_string())
			{
				dependencies.emplace_back(dependency.as_string());
			}
		}
	}

	return table(name, dependencies);
}

boost::json::object table::table::to_json() const
{
	boost::json::array dependency_array;

	std::ranges::transform(
		dependency_names,
		std::back_inserter(dependency_array),
		[](const std::string &dependency) { return boost::json::string(dependency); });

	return { { "name", boost::json::string(table_name) }, { "dependencies", dependency_array } };
}

bool table::is_valid_name(const std::string &name)
{
	if (name.empty() || name.size() > max_name_size || name == "default")
	{
		return false;
	}

	return std::ranges::all_of(
		name,
		[](char character)
		{
			return (character >= 'a' && character <= 'z') ||
				   (character >= 'A' && character <= 'Z') ||
				   (character >= '0' && character <= '9') ||
				   character == ' ' ||
				   character == '_' ||
				   character == '-';
		});
}
