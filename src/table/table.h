#ifndef TABLE_TABLE_H
#define TABLE_TABLE_H

#include <string>
#include <set>
#include <vector>

#include <boost/json.hpp>

namespace table
{
	constexpr size_t max_name_size = 64;

	struct table
	{
		bool is_valid = false;

		std::string name;

		// The table document, or the error object of an invalid one.
		boost::json::object json;

		std::string code;

		std::string message;
	};

	bool operator<(const table &lhs, const table &rhs);

	bool operator==(const table &lhs, const table &rhs);

	table parse_table(const std::string &name, const boost::json::object &json, const std::set<std::string> &tables);

	table valid_table(const std::string &name, const std::vector<std::string> &dependencies);

	table invalid_table(const std::string &code, const std::string &message);

	table to_table(const std::string &json);

	bool is_valid_name(const std::string &name);
}

#endif
