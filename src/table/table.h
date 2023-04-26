#ifndef TABLE_TABLE_H
#define TABLE_TABLE_H

#include <string>
#include <vector>
#include <boost/json.hpp>

namespace table
{
	class table
	{
	private:
		std::string name;

		std::vector<std::string> dependencies;

	public:
		table(const std::string &name, const std::vector<std::string> &dependencies);

		std::string getName() const;

		boost::json::object toJson() const;
	};

	table parseTable(boost::json::object json);
}


#endif
