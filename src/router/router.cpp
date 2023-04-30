#include "router.h"

router::router::router(repository::repository &repo):
    repository(repo)
{
}

router::response router::router::get(const std::string &route)
{
	boost::json::object body;

	if (route == "/tables")
	{
		std::set<table::valid_table> tables = repository.list_tables();
		boost::json::array tables_json;

		for (std::set<table::valid_table>::iterator it = tables.begin(); it != tables.end(); ++it)
		{
			tables_json.push_back(it->to_json());
		}

		body.insert(std::pair("tables", tables_json));
	}

	return response { boost::beast::http::status::ok, body };
}

router::response router::router::post(const std::string &route, const boost::json::object &body)
{
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

		response response = post_table_response(*table);

		delete table;

		return response;
    }

	return response { boost::beast::http::status::bad_request, boost::json::object() };
}

router::response router::router::post_table_response(const table::table &table)
{
	if (table.is_valid())
	{
		return response { boost::beast::http::status::ok, boost::json::object() };
	}
	else
	{
		return response { boost::beast::http::status::bad_request, table.to_json() };
	}
}
