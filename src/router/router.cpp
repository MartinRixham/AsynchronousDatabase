#include <set>
#include <regex>
#include <string>
#include <iostream>

#include <boost/json.hpp>
#include <boost/beast.hpp>

#include "router.h"

namespace
{
	router::response post_table_response(const table::table &table)
	{
		if (table.is_valid)
		{
			return { boost::beast::http::status::ok, boost::json::object() };
		}
		else
		{
			return { boost::beast::http::status::bad_request, table.json };
		}
	}

	router::response invalid_route_response(const std::string &route)
	{
		boost::json::object json { { "error", "Request to invalid route " + route + "." } };

		return { boost::beast::http::status::bad_request, json };
	}

	router::response valid_get_response(const boost::json::object &json)
	{
		return { boost::beast::http::status::ok, json };
	}

	std::string read_parameter(const std::string &parameters, const std::string &name)
	{
		std::stringstream in(parameters);

		std::string key;
		std::string value;

		while (key != name)
		{
			std::getline(in, key, '=');
			std::getline(in, value, '&');
		}

		return value;
	}
}

router::router::router(repository::repository &repo):
    repository(repo)
{
}

router::response router::router::get(const std::string &route) const
{
	std::smatch matches;

	if (std::regex_search(route, matches, std::regex("^/table[$?](.*)")))
	{
		std::string name = read_parameter(matches[1], "name");
		table::table table = repository.read_table(name);

		if (table.is_valid)
		{
			return { boost::beast::http::status::ok, table.json };
		}
		else
		{
			return { boost::beast::http::status::not_found, boost::json::object() };
		}
	}
	else if (route == "/tables")
	{
		const std::set<table::table> tables = repository.list_tables();
		boost::json::array tables_json;

		for (std::set<table::table>::const_iterator it = tables.begin(); it != tables.end(); ++it)
		{
			tables_json.push_back(it->json);
		}

		boost::json::object body { { "tables", tables_json } };

		return valid_get_response(body);
	}

	return invalid_route_response(route);
}

router::response router::router::post(const std::string &route, const boost::json::object &body)
{
    if (route == "/table")
    {
		const std::set<table::table> table_set = repository.list_tables();
		std::set<std::string> tables;

		for (std::set<table::table>::const_iterator it = table_set.begin(); it != table_set.end(); ++it)
		{
			tables.insert(it->name);
		}

		table::table table = table::parse_table(body, tables);

		repository.create_table(table);

		return post_table_response(table);
    }

	return invalid_route_response(route);
}
