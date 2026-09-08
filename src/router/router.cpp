#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include <boost/json.hpp>
#include <boost/beast.hpp>

#include "record/record.h"
#include "scan/scan.h"
#include "url/url.h"
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

	std::optional<boost::json::object> parse_body(const std::string &body)
	{
		if (body.empty())
		{
			return boost::json::object();
		}

		boost::system::error_code error;
		boost::json::value json = boost::json::parse(body, error);

		if (error || !json.is_object())
		{
			return std::nullopt;
		}

		return json.as_object();
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

	std::string range_query(const scan::range &range)
	{
		std::string query = "limit=" + std::to_string(range.limit);

		query += std::string("&values=") + (range.values ? "true" : "false");
		query += std::string("&reverse=") + (range.reverse ? "true" : "false");

		if (range.has_from)
		{
			query += "&from=" + url::encode(range.from);
		}

		if (range.has_to)
		{
			query += "&to=" + url::encode(range.to);
		}

		return query;
	}

	router::request forwarded_range(const router::request &request, const scan::range &range)
	{
		return { request.method, request.path, range_query(range), request.body, request.forwarded };
	}

	bool read_page(const router::response &response, std::vector<record::record> *records)
	{
		if (!response.json.contains("records") || !response.json.at("records").is_array())
		{
			return false;
		}

		const boost::json::array &answered = response.json.at("records").as_array();

		for (size_t i = 0; i < answered.size(); i++)
		{
			if (!answered[i].is_object())
			{
				continue;
			}

			const boost::json::object &json = answered[i].as_object();
			std::string value;

			if (!json.contains("key") || !json.at("key").is_string())
			{
				continue;
			}

			if (json.contains("value") && json.at("value").is_string())
			{
				value = std::string(json.at("value").as_string());
			}

			records->push_back(record::valid_record(std::string(json.at("key").as_string()), value));
		}

		return response.json.contains("next");
	}

	bool trim_to_budget(std::vector<record::record> *records)
	{
		size_t bytes = 0;

		for (size_t i = 0; i < records->size(); i++)
		{
			bytes += (*records)[i].key.size() + (*records)[i].value.size();

			if (i > 0 && bytes > scan::max_page_bytes)
			{
				records->resize(i);

				return true;
			}
		}

		return false;
	}

	void merge(std::vector<record::record> *records, bool reverse)
	{
		std::sort(
			records->begin(),
			records->end(),
			[reverse](const record::record &left, const record::record &right)
			{
				return reverse ? right.key < left.key : left.key < right.key;
			});

		records->erase(
			std::unique(
				records->begin(),
				records->end(),
				[](const record::record &left, const record::record &right) { return left.key == right.key; }),
			records->end());
	}
}

router::router::router(repository::repository &repo):
	repository(repo),
	nodes(alone)
{
}

router::router::router(repository::repository &repo, cluster::cluster &cluster_nodes):
	repository(repo),
	nodes(cluster_nodes)
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
		std::vector<cluster::member> members = nodes.members();

		if (!members.empty())
		{
			boost::json::array names;
			std::map<std::string, boost::json::array> zones;

			for (size_t i = 0; i < members.size(); i++)
			{
				names.push_back(boost::json::string(members[i].node));

				if (!members[i].zone.empty())
				{
					zones[members[i].zone].push_back(boost::json::string(members[i].node));
				}
			}

			health["nodes"] = names;

			health["leads"] = static_cast<int64_t>(nodes.leads());

			if (!zones.empty())
			{
				boost::json::object named;

				for (std::map<std::string, boost::json::array>::const_iterator it = zones.begin();
					it != zones.end();
					++it)
				{
					named[it->first] = it->second;
				}

				health["zones"] = named;
			}
		}

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
		return create_table(request, name);
	}

	if (request.method == boost::beast::http::verb::delete_)
	{
		return delete_table(request, name);
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
		return delete_records(request, name);
	}

	if (request.method != boost::beast::http::verb::get && request.method != boost::beast::http::verb::head)
	{
		return method_not_allowed(request.method);
	}

	return scan_records(request, name);
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

	cluster::placement where = request.forwarded ? cluster::placement() : nodes.replicas(record.key);

	if (request.method == boost::beast::http::verb::put || request.method == boost::beast::http::verb::delete_)
	{
		if (request.term != 0)
		{
			if (!nodes.accept(record.key, request.term))
			{
				return error_response(
					"stale_leader", "This key is led in a later term than the one that ordered this write.");
			}

			return write_record(request, name, record, where);
		}

		std::optional<cluster::leadership> lead = nodes.leader(record.key);

		if (!lead)
		{
			return write_record(request, name, record, where);
		}

		if (!lead->known)
		{
			return error_response("no_leader", "No node is leading this key's partition yet.");
		}

		if (!lead->local)
		{
			if (request.forwarded)
			{
				return error_response("no_leader", "This node does not lead this key's partition.");
			}

			return nodes.send(lead->node, request);
		}

		::router::request ordered = request;

		ordered.term = lead->term;

		std::lock_guard<std::mutex> ordering(write_lock(record.key));

		return write_record(ordered, name, record, replicas_of(request, where, record.key));
	}

	if (request.method != boost::beast::http::verb::get && request.method != boost::beast::http::verb::head)
	{
		return method_not_allowed(request.method);
	}

	if (!where.local)
	{
		return read_record(request, where.nodes);
	}

	std::optional<std::string> value = repository.read_record(name, key);

	if (value)
	{
		return text_response(boost::beast::http::status::ok, *value);
	}

	if (!where.nodes.empty())
	{
		return read_record(request, where.nodes);
	}

	return empty_response(boost::beast::http::status::not_found);
}

std::mutex &router::router::write_lock(const std::string &key)
{
	return write_locks[std::hash<std::string>()(key) % write_stripes].lock;
}

cluster::placement router::router::replicas_of(
	const request &request,
	const cluster::placement &known,
	const std::string &key)
{
	return request.forwarded ? nodes.replicas(key) : known;
}

router::response router::router::write_record(
	const request &request,
	const std::string &name,
	const record::record &record,
	const cluster::placement &where)
{
	if (where.local)
	{
		if (request.method == boost::beast::http::verb::put)
		{
			repository.write_record(name, record);
		}
		else
		{
			repository.delete_record(name, record.key);
		}
	}

	// Every copy at once, so the thread serving the write waits for the slowest of them rather
	// than for the sum of them.
	std::optional<response> refused = nodes.send_all(where.nodes, request);

	return refused ? *refused : empty_response(boost::beast::http::status::no_content);
}

router::response router::router::read_record(const request &request, const std::vector<std::string> &replicas)
{
	response answer = error_response("storage_error", "No node holding this key answered.");

	for (size_t i = 0; i < replicas.size(); i++)
	{
		answer = nodes.send(replicas[i], request);

		if (answer.status < boost::beast::http::status::internal_server_error)
		{
			return answer;
		}
	}

	return answer;
}

router::response router::router::create_table(const request &request, const std::string &name)
{
	std::optional<boost::json::object> body = parse_body(request.body);

	if (!body)
	{
		return error_response("invalid_body", "The body of a table is a JSON object.");
	}

	table::table table = table::parse_table(name, *body, table_names());

	if (!table.is_valid)
	{
		return error_response(table.code, table.message);
	}

	table::table existing = repository.read_table(name);
	response created;

	if (existing.is_valid)
	{
		if (!(existing == table))
		{
			return error_response("table_exists", "A table named \"" + name + "\" exists with different options.");
		}

		created = json_response(boost::beast::http::status::ok, table.json);
	}
	else
	{
		repository.create_table(table);

		created = json_response(boost::beast::http::status::created, table.json);
	}

	std::optional<response> failure = broadcast(request);

	return failure ? *failure : created;
}

router::response router::router::delete_table(const request &request, const std::string &name)
{
	if (!repository.has_table(name))
	{
		if (request.forwarded)
		{
			return empty_response(boost::beast::http::status::no_content);
		}

		return table_not_found(name);
	}

	repository.delete_table(name);

	std::optional<response> failure = broadcast(request);

	return failure ? *failure : empty_response(boost::beast::http::status::no_content);
}

router::response router::router::scan_records(const request &request, const std::string &name)
{
	if (!repository.has_table(name))
	{
		return table_not_found(name);
	}

	scan::range range = scan::parse_range(request.query, repository.instance());

	if (!range.is_valid)
	{
		return error_response(range.code, range.message);
	}

	scan::page page = repository.scan_records(name, range);
	std::vector<record::record> records = page.records;
	bool has_more = page.has_more;

	std::vector<std::vector<std::string>> zones = request.forwarded
		? std::vector<std::vector<std::string>>()
		: nodes.zones();
	std::optional<response> failure;

	for (size_t i = 0; i < zones.size(); i++)
	{
		std::vector<record::record> answered = page.records;
		bool more = page.has_more;

		failure = scan_zone(request, range, zones[i], &answered, &more);

		if (failure && failure->status < boost::beast::http::status::internal_server_error)
		{
			return *failure;
		}

		if (!failure)
		{
			records = answered;
			has_more = more;

			merge(&records, range.reverse);

			if (records.size() > range.limit)
			{
				records.resize(range.limit);

				has_more = true;
			}

			if (trim_to_budget(&records))
			{
				has_more = true;
			}

			break;
		}
	}

	if (failure)
	{
		return *failure;
	}

	boost::json::array records_json;

	for (size_t i = 0; i < records.size(); i++)
	{
		records_json.push_back(to_json(records[i], range.values));
	}

	boost::json::object body { { "records", records_json } };

	if (has_more && !records.empty())
	{
		body["next"] = scan::encode_cursor(records.back().key, repository.instance());
	}

	return json_response(boost::beast::http::status::ok, body);
}

std::optional<router::response> router::router::scan_zone(
	const request &request,
	const scan::range &range,
	const std::vector<std::string> &zone,
	std::vector<record::record> *records,
	bool *has_more)
{
	for (size_t i = 0; i < zone.size(); i++)
	{
		response answer = nodes.send(zone[i], forwarded_range(request, range));

		if (answer.status != boost::beast::http::status::ok)
		{
			return answer;
		}

		*has_more = read_page(answer, records) || *has_more;
	}

	return std::nullopt;
}

router::response router::router::delete_records(const request &request, const std::string &name)
{
	if (!repository.has_table(name))
	{
		return table_not_found(name);
	}

	scan::range range = scan::parse_range(request.query, repository.instance());

	if (!range.is_valid)
	{
		return error_response(range.code, range.message);
	}

	if (!range.has_from && !range.has_to)
	{
		return error_response("invalid_range", "A range delete has to name a range.");
	}

	repository.delete_records(name, range);

	std::optional<response> failure = broadcast(forwarded_range(request, range));

	return failure ? *failure : empty_response(boost::beast::http::status::no_content);
}

std::optional<router::response> router::router::broadcast(const request &request)
{
	if (request.forwarded)
	{
		return std::nullopt;
	}

	return nodes.send_all(nodes.peers(), request);
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
