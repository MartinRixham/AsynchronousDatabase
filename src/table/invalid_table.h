#ifndef TABLE_INVALID_TABLE_H
#define TABLE_INVALID_TABLE_H

#include <string>
#include <vector>
#include <boost/json.hpp>

#include "table.h"

namespace table
{
	class invalid_table : public table
	{
		std::string error;

	public:
		explicit invalid_table(const std::string &error);

		bool is_valid() const override;

		std::string get_name() const override;

		boost::json::object to_json() const override;
	};
}


#endif
