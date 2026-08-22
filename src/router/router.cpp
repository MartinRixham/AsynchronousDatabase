#include <string>

#include <boost/json.hpp>
#include <boost/beast.hpp>

#include "record/record.h"
#include "scan/scan.h"
#include "router.h"

namespace
{
	router::response not_found(const std::string &what)
	{
		return router::error_response("not_found", "No " + what + ".");
	}

	router::response table_not_found(const std::string &name)
	{
		return router::error_response("table_not_found", "No table named \"" + name + "\".");
	}

	router::response method_not_allowed(const boost::beast::http::verb &method)
	{
		return router::error_response(
			"method_not_allowed", std::string(boost::beast::http::to_string(method)) + " is not allowed here.");
	}

	boost::json::object parse_body(const std::string &body)
	{
		if (body.empty())
		{
			return boost::json::object();
		}

		return boost::json::parse(body).as_object();
	}

	boost::json::object to_json(const record::record &record, bool values)
	{
		boost::json::object json { { "key", boost::json::string(record.key) } };

		if (values)
		{
			json["value"] = boost::json::string(record.value);
		}

		return json;
	}
}

router::router::router(repository::repository &repo):
	repository(repo)
{
}

router::response router::router::route(const request &request)
{
	const std::vector<std::string> &path = request.path;

	if (path.size() == 1 && path[0] == "health")
	{
		if (request.method != boost::beast::http::verb::get && request.method != boost::beast::http::verb::head)
		{
			return method_not_allowed(request.method);
		}

		boost::json::object health { { "status", "ok" }, { "write_stalled", repository.is_write_stalled() } };

		return json_response(boost::beast::http::status::ok, health);
	}

	if (path.empty() || path[0] != "table")
	{
		return not_found("route for this path");
	}

	if (path.size() == 1)
	{
		return route_tables(request);
	}

	if (path.size() == 2)
	{
		return route_table(request, path[1]);
	}

	if (path[2] != "key")
	{
		return not_found("route for this path");
	}

	if (path.size() == 3)
	{
		return route_range(request, path[1]);
	}

	if (path.size() == 4)
	{
		return route_record(request, path[1], path[3]);
	}

	return not_found("route for this path");
}

router::response router::router::route_tables(const request &request)
{
	if (request.method != boost::beast::http::verb::get && request.method != boost::beast::http::verb::head)
	{
		return method_not_allowed(request.method);
	}

	const std::set<table::table> tables = repository.list_tables();
	boost::json::array tables_json;

	for (std::set<table::table>::const_iterator it = tables.begin(); it != tables.end(); ++it)
	{
		tables_json.push_back(it->json);
	}

	// Instances hold dozens of tables, so the list is not paged.
	return json_response(boost::beast::http::status::ok, boost::json::object { { "tables", tables_json } });
}

router::response router::router::route_table(const request &request, const std::string &name)
{
	if (request.method == boost::beast::http::verb::put)
	{
		return create_table(name, request.body);
	}

	if (request.method == boost::beast::http::verb::delete_)
	{
		if (!repository.has_table(name))
		{
			return table_not_found(name);
		}

		repository.delete_table(name);

		return empty_response(boost::beast::http::status::no_content);
	}

	if (request.method != boost::beast::http::verb::get && request.method != boost::beast::http::verb::head)
	{
		return method_not_allowed(request.method);
	}

	table::table table = repository.read_table(name);

	if (!table.is_valid)
	{
		return table_not_found(name);
	}

	return json_response(boost::beast::http::status::ok, table.json);
}

router::response router::router::route_range(const request &request, const std::string &name)
{
	if (request.method == boost::beast::http::verb::delete_)
	{
		return delete_records(name, request.query);
	}

	if (request.method != boost::beast::http::verb::get && request.method != boost::beast::http::verb::head)
	{
		return method_not_allowed(request.method);
	}

	return scan_records(name, request.query);
}

router::response router::router::route_record(
	const request &request,
	const std::string &name,
	const std::string &key)
{
	record::record record = request.method == boost::beast::http::verb::put
		? record::parse_record(key, request.body)
		: record::parse_key(key);

	if (!record.is_valid)
	{
		return error_response(record.code, record.message);
	}

	if (!repository.has_table(name))
	{
		return table_not_found(name);
	}

	if (request.method == boost::beast::http::verb::put)
	{
		repository.write_record(name, record);

		return empty_response(boost::beast::http::status::no_content);
	}

	// Deleting a key that is not there is a no op in RocksDB, and reporting 404 would mean a read
	// before every delete.
	if (request.method == boost::beast::http::verb::delete_)
	{
		repository.delete_record(name, key);

		return empty_response(boost::beast::http::status::no_content);
	}

	if (request.method != boost::beast::http::verb::get && request.method != boost::beast::http::verb::head)
	{
		return method_not_allowed(request.method);
	}

	std::optional<std::string> value = repository.read_record(name, key);

	if (!value)
	{
		return empty_response(boost::beast::http::status::not_found);
	}

	return text_response(boost::beast::http::status::ok, *value);
}

router::response router::router::create_table(const std::string &name, const std::string &body)
{
	table::table table = table::parse_table(name, parse_body(body), table_names());

	if (!table.is_valid)
	{
		return error_response(table.code, table.message);
	}

	table::table existing = repository.read_table(name);

	// Creating a table is safe to run at every start up, which is how a service should declare the
	// tables it needs, so the same options again are not a conflict.
	if (existing.is_valid)
	{
		if (existing == table)
		{
			return json_response(boost::beast::http::status::ok, table.json);
		}

		return error_response("table_exists", "A table named \"" + name + "\" exists with different options.");
	}

	repository.create_table(table);

	return json_response(boost::beast::http::status::created, table.json);
}

router::response router::router::scan_records(const std::string &name, const std::string &query)
{
	if (!repository.has_table(name))
	{
		return table_not_found(name);
	}

	scan::range range = scan::parse_range(query, repository.instance());

	if (!range.is_valid)
	{
		return error_response(range.code, range.message);
	}

	scan::page page = repository.scan_records(name, range);
	boost::json::array records;

	for (size_t i = 0; i < page.records.size(); i++)
	{
		records.push_back(to_json(page.records[i], range.values));
	}

	boost::json::object body { { "records", records } };

	// The absence of a cursor means the range is exhausted.
	if (page.has_more)
	{
		body["next"] = scan::encode_cursor(page.records.back().key, repository.instance());
	}

	return json_response(boost::beast::http::status::ok, body);
}

router::response router::router::delete_records(const std::string &name, const std::string &query)
{
	if (!repository.has_table(name))
	{
		return table_not_found(name);
	}

	scan::range range = scan::parse_range(query, repository.instance());

	if (!range.is_valid)
	{
		return error_response(range.code, range.message);
	}

	// Deleting every record in a table is deleting the table and creating it again, which drops
	// the column family instead of leaving a tombstone over everything.
	if (!range.has_from && !range.has_to)
	{
		return error_response("invalid_range", "A range delete has to name a range.");
	}

	repository.delete_records(name, range);

	return empty_response(boost::beast::http::status::no_content);
}

std::set<std::string> router::router::table_names() const
{
	const std::set<table::table> tables = repository.list_tables();
	std::set<std::string> names;

	for (std::set<table::table>::const_iterator it = tables.begin(); it != tables.end(); ++it)
	{
		names.insert(it->name);
	}

	return names;
}
