#include "router.h"

router::router::router(repository::repository &repo):
    repository(repo)
{
}

boost::json::object router::router::get(const std::string &route)
{
	boost::json::object response;

	if (route == "/tables")
	{
		std::vector<std::string> tables = repository.list_tables();

		boost::json::array tables_json;

		for (size_t i = 0; i < tables.size(); i++)
		{
			tables_json.push_back(boost::json::parse(tables[i]).as_object());
		}

		response.insert(std::pair("tables", tables_json));
	}

	return response;
}

boost::json::object router::router::post(const std::string &route, const boost::json::object &body)
{
	boost::json::object response;

    if (route == "/table")
    {
		table::table *table = table::parse_table(body);

		if (repository.has_table(*table))
		{
			std::string error = "A table with the name \"" + table->get_name() + "\" already exists.";

			response.insert(std::pair<std::string, boost::json::string>("error", boost::json::string(error)));
		}
		else
		{
			repository.create_table(*table);

			response = table->to_json();
		}

		delete table;
    }

	return response;
}
