#include <algorithm>
#include <cstdint>
#include <map>
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

	// A body that is not a JSON object is a request the client got wrong, not a failure of this
	// service, so it is answered rather than thrown: boost::json::parse throws on malformed input
	// and as_object throws on an array or a number, and either would escape as a 500.
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
	// in. Two nodes holding the same key is the copy every zone keeps, or a key whose owner
	// changed, and either way it is returned once rather than twice.
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

		// An instance standing alone names no nodes at all, rather than naming itself and looking
		// like a cluster of one.
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

			// The partitions this node orders the writes of. An election that has not settled is
			// a node leading none of them, and a write of those is refused until it has.
			health["leads"] = static_cast<int64_t>(nodes.leads());

			// The zones are what say how many copies of a record there are, so a cluster that has
			// them names them. One that has none says nothing rather than naming a zone of "".
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

	// A record lives on the one node the hash of its partition names in each zone, and a node
	// holding no copy of it answers by asking one that does. The key alone decides which nodes
	// those are, so the same key of two tables is on the same nodes and is one hop.
	//
	// A request that arrived forwarded is served here whatever this node makes of the membership:
	// the node that sent it has already decided who holds the key.
	cluster::placement where = request.forwarded ? cluster::placement() : nodes.replicas(record.key);

	if (request.method == boost::beast::http::verb::put || request.method == boost::beast::http::verb::delete_)
	{
		// A write travels in two hops, and the term is what tells them apart: a write *to* the
		// leader carries none, because the node sending it is only asking for it to be ordered,
		// and a write *from* the leader carries the term it was ordered in.
		//
		// This one was ordered, so it is applied here — unless this node has already applied a
		// write of a later term, which is a leader that has been replaced and does not know it.
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

		// No leadership at all is a cluster that orders nothing — one instance standing alone, or
		// one that has no zones — and a write is written the way it always was.
		if (!lead)
		{
			return write_record(request, name, record, where);
		}

		// A partition nothing leads is a partition whose writes have nowhere to be ordered. Saying
		// so is what makes the client run the write again once one is elected, which takes as long
		// as the lease of the leader that went away.
		if (!lead->known)
		{
			return error_response("no_leader", "No node is leading this key's partition yet.");
		}

		if (!lead->local)
		{
			// A write that arrived here to be ordered, at a node that does not order it, is two
			// nodes disagreeing about the leader. It is refused rather than passed on again, so
			// that a disagreement cannot bounce a write between them.
			if (request.forwarded)
			{
				return error_response("no_leader", "This node does not lead this key's partition.");
			}

			return nodes.send(lead->node, request);
		}

		// This node leads it, so this is where the order is decided, whether the write arrived
		// here from a client or from a node that knew who to ask. Every copy is told the term.
		//
		// The namespace is hidden here by the name of the class, and the name of the type by the
		// name of the parameter.
		::router::request ordered = request;

		ordered.term = lead->term;

		return write_record(ordered, name, record, nodes.replicas(record.key));
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

	// A key this node holds nothing for may still be in another zone. A node that was replaced, or
	// a zone that came back, holds none of what was written while it was away, and it is the owner
	// of those keys in its own zone all the same — so a miss here is asked of the copies elsewhere
	// rather than answered as though the record had never been written.
	if (!where.nodes.empty())
	{
		return read_record(request, where.nodes);
	}

	return empty_response(boost::beast::http::status::not_found);
}

// Every copy of a record is written before the write is answered, so a record is in every zone by
// the time the client is told it is written, or the client is told it is not. Writing a record is
// idempotent — a key and a value, or a key that is gone — so a failure is a request to run again.
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
		// Deleting a key that is not there is a no op in RocksDB, and reporting 404 would mean a
		// read before every delete.
		else
		{
			repository.delete_record(name, record.key);
		}
	}

	for (size_t i = 0; i < where.nodes.size(); i++)
	{
		response answer = nodes.send(where.nodes[i], request);

		if (answer.status >= boost::beast::http::status::bad_request)
		{
			return answer;
		}
	}

	return empty_response(boost::beast::http::status::no_content);
}

// A node that does not answer is a copy to pass over rather than an answer to give, which is what
// replication is for: any one of the zones can be gone and the record is still read. What a node
// that did answer said is the answer, a 404 included: every zone is written before a write is
// answered, so one copy saying a key is not there is enough to say it is not there.
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

	// The keys of a table are spread across the cluster, but every zone holds a copy of all of
	// them, so a scan is this node's own share and *one* zone's answer for the rest, put back into
	// key order — not every node's, which would be the same keys once per zone. This node's own
	// zone is asked first, because a page from it crosses no zone boundary.
	std::vector<std::vector<std::string>> zones = request.forwarded
		? std::vector<std::vector<std::string>>()
		: nodes.zones();
	std::optional<response> failure;

	for (size_t i = 0; i < zones.size(); i++)
	{
		std::vector<record::record> answered = page.records;
		bool more = page.has_more;

		failure = scan_zone(request, range, zones[i], &answered, &more);

		// A zone that answered with something other than a page, and not because a node of it was
		// unreachable, is every zone's answer: a cursor this instance did not issue is refused by
		// all of them, so asking the next one would only be slower.
		if (failure && failure->status < boost::beast::http::status::internal_server_error)
		{
			return *failure;
		}

		if (!failure)
		{
			records = answered;
			has_more = more;

			merge(&records, range.reverse);

			// What is over the limit is dropped rather than held: the next page asks again from
			// where this one ended, and the keys dropped here are the keys it starts with.
			if (records.size() > range.limit)
			{
				records.resize(range.limit);

				has_more = true;
			}

			break;
		}
	}

	// Every zone had a node that did not answer, so there is no zone left that holds the range.
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

	// The absence of a cursor means the range is exhausted.
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
