#include "invalid_table.h"

#include "error.h"

table::invalid_table::invalid_table(const std::string &error):
	error(error)
{
}

bool table::invalid_table::is_valid() const
{
	return false;
}

std::string table::invalid_table::get_name() const
{
	throw std::runtime_error(ERROR(""));
}

boost::json::object table::invalid_table::to_json() const
{
	boost::json::object json;

	json.insert(std::pair<std::string, boost::json::string>("error", error));

	return json;
}
