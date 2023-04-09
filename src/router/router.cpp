#include "router.h"

#include <utility>

router::router::router(repository::repository &repo):
    repository(repo)
{
}

boost::json::object router::router::get(const std::string &route)
{
	boost::json::object json;

	if (route == "tables")
	{
		std::vector<std::string> tables = repository.list_tables();

		boost::json::array tables_json(tables.begin(), tables.end());

		json.insert(std::make_pair("tables", tables_json));
	}

	return json;
}

void router::router::post(const std::string &route, const std::string &body)
{
    if (route == "table")
    {
	    repository.create_table(body);
    }
}
