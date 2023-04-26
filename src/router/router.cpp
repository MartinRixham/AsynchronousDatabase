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

		response.insert(std::make_pair("tables", tables_json));
	}

	return response;
}

boost::json::object router::router::post(const std::string &route, const boost::json::object &body)
{
	boost::json::object response;

    if (route == "/table")
    {
		std::string name = std::string(body.at("name").as_string());

		if (name.length() == 0)
		{
			response.insert(std::make_pair<std::string, boost::json::string>("error", "Table requires name of length greater than 0."));
		}
		else if (repository.has_table(name))
		{
			std::string error = "A table with the name \"" + name + "\" already exists.";

			response.insert(std::make_pair<std::string, boost::json::string>("error", boost::json::string(error)));
		}
		else
		{
			repository.create_table(table::parseTable(body));
		}
    }

	return response;
}
