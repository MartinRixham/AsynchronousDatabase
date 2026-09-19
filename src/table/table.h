#pragma once

#include <expected>
#include <string>
#include <set>
#include <vector>

#include <boost/json.hpp>

#include "error/error_message.h"

namespace table
{
	constexpr size_t max_name_size = 64;

	class table
	{
		std::string table_name;

		std::vector<std::string> dependency_names;

	public:
		table(const std::string &name, const std::vector<std::string> &dependencies);

		const std::string &name() const;

		boost::json::object to_json() const;

		bool operator==(const table &other) const = default;
	};

	bool operator<(const table &lhs, const table &rhs);

	[[nodiscard]] std::expected<table, error::error_message> parse_table(
		const std::string &name,
		const boost::json::object &json,
		const std::set<std::string> &tables);

	table to_table(const boost::json::object &json);

	bool is_valid_name(const std::string &name);
}
