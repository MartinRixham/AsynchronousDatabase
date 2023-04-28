#ifndef TABLE_VALID_TABLE_H
#define TABLE_VALID_TABLE_H

#include <string>
#include <vector>
#include <boost/json.hpp>

#include "table.h"

namespace table
{
	class valid_table : public table
	{
		std::string name;

		std::vector<std::string> dependencies;

	public:
		valid_table(const std::string &name, const std::vector<std::string> &dependencies);

		bool is_valid() const override;

		std::string get_name() const override;

		boost::json::object to_json() const override;
	};
}


#endif
