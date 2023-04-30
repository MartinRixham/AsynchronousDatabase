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
		std::set<table::valid_table> tables = repository.list_tables();
		boost::json::array tables_json;

		for (std::set<table::valid_table>::iterator it = tables.begin(); it != tables.end(); ++it)
		{
			tables_json.push_back(it->to_json());
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
		std::set<table::valid_table> table_set = repository.list_tables();
		std::set<std::string> tables;

		for (std::set<table::valid_table>::iterator it = table_set.begin(); it != table_set.end(); ++it)
		{
			tables.insert(it->get_name());
		}

		table::table *table = table::parse_table(body, tables);

		repository.create_table(*table);

		response = table->to_json();

		delete table;
    }

	return response;
}
