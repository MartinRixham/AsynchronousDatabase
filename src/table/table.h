#ifndef TABLE_TABLE_H
#define TABLE_TABLE_H

#include <string>
#include <set>
#include <vector>

#include <boost/json.hpp>

namespace table
{
	struct table
	{
		bool is_valid;

		std::string name;

		boost::json::object json;
	};

	bool operator <(const table &lhs, const table &rhs);

	table parse_table(const boost::json::object &json, const std::set<std::string> &tables);

	table valid_table(const std::string &name, const std::vector<std::string> &dependencies);

	table invalid_table(const std::string &error);

	table to_table(const std::string &json);
}

#endif
