#ifndef TABLE_TABLE_H
#define TABLE_TABLE_H

#include <string>
#include <vector>
#include <boost/json.hpp>

namespace table
{
	class table
	{
	public:
		virtual bool is_valid() const = 0;

		virtual std::string get_name() const = 0;

		virtual boost::json::object to_json() const = 0;

		virtual ~table() = default;
	};

	table *parse_table(boost::json::object json);
}


#endif
