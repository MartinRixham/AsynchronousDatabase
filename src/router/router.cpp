#include <algorithm>
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

	// The range as another node is asked for it. A cursor and a prefix have already been resolved
	// into bounds here, and a cursor of this node's is one no other node would take.
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

	// A page as another node answered it. Its own cursor is thrown away: the cursor a client is
	// given is issued by the node it asked, and names a position in the merged order.
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

	// The pages of every node in one order, which is the order a single node would have answered
	// in. Two nodes holding the same key is a key whose owner changed, and the copy left behind is
	// passed over rather than returned twice.
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
		std::vector<std::string> members = nodes.members();

		// An instance standing alone names no nodes at all, rather than naming itself and looking
		// like a cluster of one.
		if (!members.empty())
		{
			boost::json::array names;

			for (size_t i = 0; i < members.size(); i++)
			{
				names.push_back(boost::json::string(members[i]));
			}

			health["nodes"] = names;
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

	// Every node holds every table, so the tables are read where they are asked for.
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

	// A record lives on the one node the hash of its key names, and every other node answers for
	// it by asking that one. The key alone decides, so the same key of two tables is one hop.
	if (!request.forwarded)
	{
		std::optional<std::string> owner = nodes.owner(record.key);

		if (owner)
		{
			return nodes.send(*owner, request);
		}
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

router::response router::router::create_table(const request &request, const std::string &name)
{
	table::table table = table::parse_table(name, parse_body(request.body), table_names());

	if (!table.is_valid)
	{
		return error_response(table.code, table.message);
	}

	table::table existing = repository.read_table(name);
	response created;

	// Creating a table is safe to run at every start up, which is how a service should declare the
	// tables it needs, so the same options again are not a conflict.
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

	// A record is written to the node that owns its key, and that node has to have somewhere to
	// put it, so a table belongs to every node rather than to the one it was created on.
	std::optional<response> failure = broadcast(request);

	return failure ? *failure : created;
}

router::response router::router::delete_table(const request &request, const std::string &name)
{
	if (!repository.has_table(name))
	{
		// A node that never had the table has nothing to say about a deletion the rest of the
		// cluster is carrying out.
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
	std::vector<std::string> peers = request.forwarded
		? std::vector<std::string>()
		: nodes.peers();

	// The keys of a table are spread across the cluster, so a scan is the same range asked of
	// every node and the answers put back into key order.
	for (size_t i = 0; i < peers.size(); i++)
	{
		response answer = nodes.send(peers[i], forwarded_range(request, range));

		if (answer.status != boost::beast::http::status::ok)
		{
			return answer;
		}

		has_more = read_page(answer, &records) || has_more;
	}

	if (!peers.empty())
	{
		merge(&records, range.reverse);

		// What is over the limit is dropped rather than held: the next page asks every node again
		// from where this one ended, and the keys dropped here are the keys it starts with.
		if (records.size() > range.limit)
		{
			records.resize(range.limit);

			has_more = true;
		}
	}

	boost::json::array records_json;

	for (size_t i = 0; i < records.size(); i++)
	{
		records_json.push_back(to_json(records[i], range.values));
	}

	boost::json::object body { { "records", records_json } };

	// The absence of a cursor means the range is exhausted.
	if (has_more && !records.empty())
	{
		body["next"] = scan::encode_cursor(records.back().key, repository.instance());
	}

	return json_response(boost::beast::http::status::ok, body);
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

	// Deleting every record in a table is deleting the table and creating it again, which drops
	// the column family instead of leaving a tombstone over everything.
	if (!range.has_from && !range.has_to)
	{
		return error_response("invalid_range", "A range delete has to name a range.");
	}

	repository.delete_records(name, range);

	// The range is deleted on every node, because every node holds a share of the keys in it.
	std::optional<response> failure = broadcast(forwarded_range(request, range));

	return failure ? *failure : empty_response(boost::beast::http::status::no_content);
}

std::optional<router::response> router::router::broadcast(const request &request)
{
	if (request.forwarded)
	{
		return std::nullopt;
	}

	std::vector<std::string> peers = nodes.peers();

	for (size_t i = 0; i < peers.size(); i++)
	{
		response answer = nodes.send(peers[i], request);

		// A node that refused, or that did not answer at all, is a node the cluster now disagrees
		// with, and saying so is what lets a client run the same request again.
		if (answer.status >= boost::beast::http::status::bad_request)
		{
			return answer;
		}
	}

	return std::nullopt;
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
