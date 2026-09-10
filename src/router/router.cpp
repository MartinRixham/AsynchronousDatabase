#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <boost/json.hpp>
#include <boost/beast.hpp>
#include <boost/lexical_cast/try_lexical_convert.hpp>

#include "base64/base64.h"
#include "cluster/partition.h"
#include "record/record.h"
#include "repository/repository.h"
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

	router::response immutable_table(const std::string &name)
	{
		return router::error_response(
			"table_immutable", "Table \"" + name + "\" is immutable, and nothing it holds is deleted.");
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
		boost::json::object json { { "key", boost::json::string(record::partition_key(record.key)) } };
		std::string_view sort = record::sort_key(record.key);

		if (!sort.empty())
		{
			json["sort"] = boost::json::string(sort);
		}

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

	scan::page read_page(const router::response &response)
	{
		scan::page page;

		if (!response.json.contains("records") || !response.json.at("records").is_array())
		{
			return page;
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
			std::string sort;

			if (!json.contains("key") || !json.at("key").is_string())
			{
				continue;
			}

			if (json.contains("value") && json.at("value").is_string())
			{
				value = std::string(json.at("value").as_string());
			}

			if (json.contains("sort") && json.at("sort").is_string())
			{
				sort = std::string(json.at("sort").as_string());
			}

			page.records.push_back(
				record::valid_record(
					record::compose_key(std::string(json.at("key").as_string()), sort), value));
		}

		page.has_more = response.json.contains("next");

		return page;
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

	// A node asking this one for its share of a table, and where to cut a walk of it up. Both are
	// answered from this store alone and are never forwarded: what is being asked for is what this
	// node holds, and no other node has it.
	if (path.size() == 3 && path[2] == "file")
	{
		return route_file(request, path[1]);
	}

	if (path.size() == 3 && path[2] == "split")
	{
		return route_split(request, path[1]);
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
		return route_record(request, path[1], path[3], "");
	}

	if (path.size() == 5)
	{
		return route_record(request, path[1], path[3], path[4]);
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

router::response router::router::route_file(const request &request, const std::string &name)
{
	if (request.method != boost::beast::http::verb::get)
	{
		return method_not_allowed(request.method);
	}

	if (!repository.has_table(name))
	{
		return table_not_found(name);
	}

	std::optional<cluster::partition_set> partitions =
		cluster::decode_partitions(url::read_parameter(request.query, "partitions"));

	if (!partitions)
	{
		return error_response(
			"invalid_partitions", "The partitions asked for are not a set of the partitions this cluster has.");
	}

	repository::share wanted;

	wanted.partitions = *partitions;

	// The keys alone, for a node deciding what it may give up rather than taking records over. It
	// is the same parameter a scan takes and defaults the same way.
	wanted.values = url::read_parameter(request.query, "values") != "false";

	std::string cursor = url::read_parameter(request.query, "from");

	if (!cursor.empty())
	{
		std::optional<std::string> resumed = base64::decode(cursor);

		if (!resumed)
		{
			return error_response("invalid_cursor", "The key to resume the file at is not base64.");
		}

		wanted.from = *resumed;
		wanted.has_from = true;
	}

	std::string end = url::read_parameter(request.query, "to");

	if (!end.empty())
	{
		std::optional<std::string> last = base64::decode(end);

		if (!last)
		{
			return error_response("invalid_cursor", "The key to end the file at is not base64.");
		}

		wanted.to = *last;
		wanted.has_to = true;
	}

	size_t bytes = 0;

	// How much of the table to walk is the asking node's to say, because it is the asking node
	// that holds the file: a walk that runs in several pieces at once shares the budget out
	// between them. What it may not do is ask for more than this node will build.
	if (boost::conversion::try_lexical_convert(url::read_parameter(request.query, "bytes"), bytes) && bytes > 0)
	{
		wanted.bytes = std::min(bytes, repository::max_file_bytes);
	}

	repository::extract taken = repository.export_records(name, wanted);

	return file_response(taken.file, taken.records, taken.has_more ? base64::encode(taken.last) : "");
}

// Where this node would cut a walk of its own table up, so that a node reading it can ask for
// several pieces of it at once. It is this store's answer and nobody else's: the pieces are only
// as even as what this node holds, which is what a walk of this node is bounded by.
router::response router::router::route_split(const request &request, const std::string &name)
{
	if (request.method != boost::beast::http::verb::get)
	{
		return method_not_allowed(request.method);
	}

	if (!repository.has_table(name))
	{
		return table_not_found(name);
	}

	size_t ways = 0;

	if (!boost::conversion::try_lexical_convert(url::read_parameter(request.query, "ways"), ways) || ways < 1)
	{
		return error_response("invalid_range", "The number of ways to cut a table up is not a number.");
	}

	boost::json::array keys;
	std::vector<std::string> points = repository.split_points(name, std::min(ways, scan::max_limit));

	for (size_t i = 0; i < points.size(); i++)
	{
		keys.push_back(boost::json::string(base64::encode(points[i])));
	}

	return json_response(boost::beast::http::status::ok, boost::json::object { { "keys", keys } });
}

router::response router::router::route_record(
	const request &request,
	const std::string &name,
	const std::string &partition,
	const std::string &sort)
{
	std::string key = record::compose_key(partition, sort);
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
		// Whichever node the delete lands on refuses it. Every node holds the table document, so
		// none of them needs the key, a copy of it or a hop to a leader to answer.
		if (request.method == boost::beast::http::verb::delete_ && repository.read_table(name).immutable)
		{
			return immutable_table(name);
		}

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
			std::optional<response> refused = refuse_overwrite(request, name, record.key);

			return refused ? *refused : write_record(request, name, record, where);
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

		std::optional<response> refused = refuse_overwrite(request, name, record.key);

		if (refused)
		{
			return *refused;
		}

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

std::optional<router::response> router::router::refuse_overwrite(
	const request &request,
	const std::string &name,
	const std::string &key)
{
	if (request.method != boost::beast::http::verb::put || !repository.read_table(name).immutable)
	{
		return std::nullopt;
	}

	if (!repository.read_record(name, key))
	{
		return std::nullopt;
	}

	return error_response("record_exists", "Table \"" + name + "\" is immutable and already holds this key.");
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
		zone_answer answered = scan_zone(request, range, zones[i]);

		failure = answered.failure;

		if (failure && failure->status < boost::beast::http::status::internal_server_error)
		{
			return *failure;
		}

		if (!failure)
		{
			records = page.records;

			records.insert(records.end(), answered.page.records.begin(), answered.page.records.end());

			has_more = page.has_more || answered.page.has_more;

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

router::router::zone_answer router::router::scan_zone(
	const request &request,
	const scan::range &range,
	const std::vector<std::string> &zone)
{
	zone_answer answered;

	for (size_t i = 0; i < zone.size(); i++)
	{
		response answer = nodes.send(zone[i], forwarded_range(request, range));

		if (answer.status != boost::beast::http::status::ok)
		{
			answered.failure = answer;

			return answered;
		}

		scan::page read = read_page(answer);

		answered.page.records.insert(answered.page.records.end(), read.records.begin(), read.records.end());
		answered.page.has_more = read.has_more || answered.page.has_more;
	}

	return answered;
}

router::response router::router::delete_records(const request &request, const std::string &name)
{
	if (!repository.has_table(name))
	{
		return table_not_found(name);
	}

	if (repository.read_table(name).immutable)
	{
		return immutable_table(name);
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
