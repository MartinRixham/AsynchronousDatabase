#include "router.h"

router::router::router(repository::repository &repo):
    repository(repo)
{
}

boost::json::object router::router::get(const std::string &route)
{
	boost::json::object json;

	if (route == "/tables")
	{
		std::vector<std::string> tables = repository.list_tables();

		boost::json::array tables_json;

		for (size_t i = 0; i < tables.size(); i++)
		{
			tables_json.push_back(boost::json::parse(tables[i]).as_object());
		}

		json.insert(std::make_pair("tables", tables_json));
	}

	return json;
}

void router::router::post(const std::string &route, const std::string &body)
{
    if (route == "/table")
    {
		boost::json::object table = boost::json::parse(body).as_object();
	    repository.create_table(std::string(table["name"].as_string()));
    }
}
