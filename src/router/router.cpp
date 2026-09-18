#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
		return router::error_response(error::code::not_found, "No " + what + ".");
	}

	router::response table_not_found(const std::string &name)
	{
		return router::error_response(error::code::table_not_found, "No table named \"" + name + "\".");
	}

	// The request as the node that ordered it sends it on, which is the version it ordered it in
	// and nothing else changed.
	router::request carried(const router::request &request, int64_t term, uint64_t count = 0)
	{
		router::request ordered = request;

		ordered.term = term;
		ordered.count = count;

		return ordered;
	}

	router::response node_incomplete()
	{
		return router::error_response(
			error::code::node_incomplete,
			"This node holds less than it owns, so it cannot say a key is missing.");
	}

	router::response node_alone()
	{
		return router::error_response(
			error::code::node_alone,
			"This node has no membership but itself, so it cannot say which node holds a key.");
	}

	// A route that waits on nothing, as the answer of one that does.
	boost::asio::awaitable<router::response> answered(router::response answer)
	{
		co_return std::move(answer);
	}

	router::response method_not_allowed(const boost::beast::http::verb &method)
	{
		return router::error_response(
			error::code::method_not_allowed,
			std::string(boost::beast::http::to_string(method)) + " is not allowed here.");
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

	// The records of a page, and the cursor to resume at when there are more of them. The cursor
	// carries the partition it was issued for, so it can only be given back for this same scan.
	router::response page_response(const scan::page &page, const scan::range &range, const std::string &instance)
	{
		boost::json::array records;

		std::ranges::transform(
			page.records,
			std::back_inserter(records),
			[&range](const record::record &paged) { return to_json(paged, range.values); });

		boost::json::object body { { "records", records } };

		if (page.has_more && !page.records.empty())
		{
			body["next"] = scan::encode_cursor(page.records.back().key, instance, range.partition);
		}

		return router::json_response(boost::beast::http::status::ok, body);
	}
}

router::router::router(
	repository::repository &repo,
	cluster::cluster &cluster_nodes,
	const cluster::forwarder &node_forwarding):
	repository(repo),
	nodes(cluster_nodes),
	forwarding(node_forwarding)
{
	cluster::partition_terms applied = repository.read_terms();

	// The tables are fenced by the partition of cluster::table_key, and every name in the schema
	// already carries the term its create or delete was ordered in.
	table::schema schema = repository.read_schema();
	std::set<std::string> names = schema.names();
	int64_t &tables = applied[cluster::partition_of(cluster::table_key)];

	for (const auto &table_name : names)
	{
		std::optional<table::entry> entry = schema.read_entry(table_name);

		if (entry && entry->stamp.term > tables)
		{
			tables = entry->stamp.term;
		}
	}

	nodes.restore_terms(applied);
}

bool router::router::is_incomplete() const
{
	return (nodes.holdings() & ~nodes.vouched()).any();
}

// A node that came up short of its share is a node whose tables may be the ones it never read, so
// what it does not hold is unknown to it rather than absent.
router::response router::router::missing_table(const std::string &name) const
{
	return is_incomplete() ? node_incomplete() : table_not_found(name);
}

bool router::router::is_short_of(size_t partition) const
{
	return nodes.holdings().test(partition) && !nodes.vouched().test(partition);
}

void router::router::is_draining(bool value) noexcept
{
	draining = value;
}

bool router::router::is_draining() const noexcept
{
	return draining;
}

boost::asio::awaitable<router::response> router::router::route(const request &request)
{
	const std::vector<std::string> &path = request.path;

	if (path.size() == 1 && path[0] == "health")
	{
		return answered(route_health(request));
	}

	// The whole schema, versions and tombstones and all, for a node filling a store or reconciling
	// one against the cluster. `GET /table` is the client's view of the same thing and carries
	// neither, because a client has no use for a name that is gone.
	if (path.size() == 1 && path[0] == "schema")
	{
		if (request.method != boost::beast::http::verb::get)
		{
			return answered(method_not_allowed(request.method));
		}

		return answered(json_response(boost::beast::http::status::ok, repository.read_schema().json()));
	}

	if (path.empty() || path[0] != "table")
	{
		return answered(not_found("route for this path"));
	}

	if (path.size() == 1)
	{
		return answered(route_tables(request));
	}

	if (path.size() == 2)
	{
		return answered(route_table(request, path[1]));
	}

	// A node asking this one for its share of a table, and where to cut a walk of it up. Both are
	// answered from this store alone and are never forwarded: what is being asked for is what this
	// node holds, and no other node has it.
	if (path.size() == 3 && path[2] == "file")
	{
		return answered(route_file(request, path[1]));
	}

	if (path.size() == 3 && path[2] == "split")
	{
		return answered(route_split(request, path[1]));
	}

	if (path[2] != "key")
	{
		return answered(not_found("route for this path"));
	}

	if (path.size() == 3)
	{
		return route_range(request, path[1]);
	}

	if (path.size() != 4 && path.size() != 5)
	{
		return answered(not_found("route for this path"));
	}

	if (request.forwarded)
	{
		return answered(route_forwarded_record(request, path[1], path[3], path.size() == 5 ? path[4] : ""));
	}
	else
	{
		return route_record(request, path[1], path[3], path.size() == 5 ? path[4] : "");
	}
}

router::response router::router::route_health(const request &request)
{
	if (request.method != boost::beast::http::verb::get && request.method != boost::beast::http::verb::head)
	{
		return method_not_allowed(request.method);
	}

	bool unled = nodes.is_unled();
	bool leaving = draining.load();

	boost::json::object health { { "status", "ok" },
								 { "write_stalled", repository.is_write_stalled() },
								 { "incomplete", is_incomplete() },
								 { "unled", unled },
								 { "draining", leaving } };

	// Where this node reads the membership from, and whether it is still in it. An instance
	// that was told no etcd names none, the way it names no nodes.
	cluster::etcd_registration registration = nodes.registration();

	if (registration.configured)
	{
		health["etcd"] =
			boost::json::object { { "registered", registration.held }, { "endpoint", registration.endpoint } };
	}

	std::vector<cluster::member> members = nodes.members();

	if (!members.empty())
	{
		boost::json::array names;
		std::map<std::string, boost::json::array> zones;

		for (const auto &listed : members)
		{
			names.push_back(boost::json::string(listed.node));

			if (!listed.zone.empty())
			{
				zones[listed.zone].push_back(boost::json::string(listed.node));
			}
		}

		health["nodes"] = names;

		health["leads"] = static_cast<int64_t>(nodes.leads());

		if (!zones.empty())
		{
			boost::json::object named;

			for (const auto &[zone, zone_nodes] : zones)
			{
				named[zone] = zone_nodes;
			}

			health["zones"] = named;
		}
	}

	// The document says what the process is doing and the status says whether to send it
	// anything: a node that takes no writes is one a load balancer should stop choosing, and
	// this is the only place it can be told. It goes on serving the keys it holds and
	// answering its peers, which do not reach it this way.
	return json_response(
		unled || leaving ? boost::beast::http::status::service_unavailable : boost::beast::http::status::ok,
		health);
}

router::response router::router::route_tables(const request &request)
{
	if (request.method != boost::beast::http::verb::get && request.method != boost::beast::http::verb::head)
	{
		return method_not_allowed(request.method);
	}

	const std::set<table::table> tables = repository.list_tables();
	boost::json::array tables_json;

	std::ranges::transform(tables, std::back_inserter(tables_json), &table::table::json);

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

boost::asio::awaitable<router::response> router::router::route_range(const request &request, const std::string &name)
{
	if (request.method != boost::beast::http::verb::get && request.method != boost::beast::http::verb::head)
	{
		return answered(method_not_allowed(request.method));
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
			error::code::invalid_partitions,
			"The partitions asked for are not a set of the partitions this cluster has.");
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
			return error_response(error::code::invalid_cursor, "The key to resume the file at is not base64.");
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
			return error_response(error::code::invalid_cursor, "The key to end the file at is not base64.");
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

	return file_response(std::move(taken.file), taken.records, taken.has_more ? base64::encode(taken.last) : "");
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
		return error_response(error::code::invalid_range, "The number of ways to cut a table up is not a number.");
	}

	boost::json::array keys;
	std::vector<std::string> points = repository.split_points(name, std::min(ways, scan::max_limit));

	std::ranges::transform(
		points,
		std::back_inserter(keys),
		[](const std::string &point) { return boost::json::string(base64::encode(point)); });

	return json_response(boost::beast::http::status::ok, boost::json::object { { "keys", keys } });
}

boost::asio::awaitable<router::response> router::router::route_record(
	const request &request,
	const std::string &name,
	const std::string &partition,
	const std::string &sort)
{
	bool writing = request.method == boost::beast::http::verb::put;

	if (!writing && request.method != boost::beast::http::verb::get && request.method != boost::beast::http::verb::head)
	{
		return answered(method_not_allowed(request.method));
	}

	std::string key = record::compose_key(partition, sort);
	record::record record = writing ? record::parse_record(key, request.body) : record::parse_key(key);

	if (!record.is_valid)
	{
		return answered(error_response(record.code, record.message));
	}

	// A node with no membership but itself answers every key out of a store holding its share, so
	// its 404 may be another node's record. A peer forwarding here is asking what this store holds,
	// which it can still say.
	if (!writing && nodes.is_alone())
	{
		return answered(node_alone());
	}

	if (!repository.has_table(name))
	{
		return answered(missing_table(name));
	}

	if (!writing)
	{
		cluster::placement where = nodes.replicas(record.key);

		if (!where.local)
		{
			return read_record(request, std::move(where));
		}

		std::optional<std::string> value = repository.read_record(name, key);

		if (value)
		{
			return answered(text_response(boost::beast::http::status::ok, *value));
		}

		if (!where.nodes.empty())
		{
			return read_record(request, std::move(where));
		}

		return answered(read_here(name, key));
	}

	cluster::leadership lead = nodes.leader(record.key);

	if (lead.local)
	{
		return answered(lead_write(request, name, std::move(record), lead));
	}

	if (!lead.known)
	{
		return answered(error_response(error::code::no_leader, "No node is leading this key's partition yet."));
	}

	return forward_to_leader(request, std::move(lead));
}

router::response router::router::route_forwarded_record(
	const request &request,
	const std::string &name,
	const std::string &partition,
	const std::string &sort)
{
	bool writing = request.method == boost::beast::http::verb::put;

	if (!writing && request.method != boost::beast::http::verb::get && request.method != boost::beast::http::verb::head)
	{
		return method_not_allowed(request.method);
	}

	std::string key = record::compose_key(partition, sort);
	record::record record = writing ? record::parse_record(key, request.body) : record::parse_key(key);

	if (!record.is_valid)
	{
		return error_response(record.code, record.message);
	}

	if (!writing)
	{
		return read_here(name, key);
	}

	// A write another node ordered is applied here even where this node takes itself for the
	// leader: claiming raised this node's term, so the term decides which of the two is stale, and
	// applying carries nothing on.
	if (request.term != 0)
	{
		return apply_write(request, name, std::move(record));
	}

	cluster::leadership lead = nodes.leader(record.key);

	if (!lead.local)
	{
		return apply_write(request, name, std::move(record));
	}

	return lead_write(request, name, std::move(record), lead);
}

router::response router::router::read_here(const std::string &name, const std::string &key) const
{
	std::optional<std::string> value = repository.read_record(name, key);

	if (value)
	{
		return text_response(boost::beast::http::status::ok, *value);
	}

	// A partition this node has just been handed is one it holds nothing of until it has fetched
	// it, so a miss there is a record it may never have received.
	if (is_short_of(cluster::partition_of(key)))
	{
		return node_incomplete();
	}

	return empty_response(boost::beast::http::status::not_found);
}

router::response router::router::apply_write(const request &request, const std::string &name, record::record record)
{
	// Nothing carries a write in no term, so one without a term is a client's, sent here to be
	// ordered by a node that took this one for the leader.
	if (request.term == 0)
	{
		return error_response(error::code::no_leader, "This node does not lead this key's partition.");
	}

	if (!nodes.accept(record.key, request.term))
	{
		return error_response(
			error::code::stale_leader,
			"This key is led in a later term than the one that ordered this write.");
	}

	// The version the leader stamped, applied as it was given rather than made again here: the
	// copies of one write are the same record and have to say so.
	record.stamp = record::version { request.term, request.count };

	repository.write_record(name, record);

	return empty_response(boost::beast::http::status::no_content);
}

router::response router::router::lead_write(
	const request &request,
	const std::string &name,
	record::record record,
	const cluster::leadership &lead)
{
	record.stamp = record::version { lead.term, repository.next_count() };

	cluster::placement where = nodes.replicas(record.key);

	if (where.local)
	{
		repository.write_record(name, record);
	}

	// Every copy at once, so the thread serving the write waits for the slowest of them rather
	// than for the sum of them.
	std::optional<response> refused =
		cluster::refusal(forwarding.forward_all(where.nodes, carried(request, lead.term, record.stamp.count)));

	return refused ? *refused : empty_response(boost::beast::http::status::no_content);
}

boost::asio::awaitable<router::response> router::router::forward_to_leader(
	const request &request,
	cluster::leadership lead)
{
	co_return co_await forwarding.async_forward(lead.node, request);
}

boost::asio::awaitable<router::response> router::router::read_record(const request &request, cluster::placement where)
{
	response answer = error_response(error::code::storage_error, "No node holding this key answered.");

	for (const auto &replica : where.nodes)
	{
		answer = co_await forwarding.async_forward(replica, request);

		if (answer.status < boost::beast::http::status::internal_server_error)
		{
			co_return answer;
		}
	}

	co_return answer;
}

router::router::ordering router::router::order_schema(const request &request)
{
	// A term is a leader having ordered this already, so the node it was sent to carries it out
	// and no further.
	if (request.term != 0)
	{
		if (!nodes.accept(cluster::table_key, request.term))
		{
			return ordering { error_response(
								  error::code::stale_leader,
								  "The tables are led in a later term than the one that ordered this."),
							  {},
							  0 };
		}

		return ordering { std::nullopt, {}, 0, true };
	}

	cluster::leadership lead = nodes.leader(cluster::table_key);

	if (!lead.known)
	{
		return ordering { error_response(error::code::no_leader, "No node is leading the tables yet."), {}, 0 };
	}

	if (!lead.local)
	{
		if (request.forwarded)
		{
			return ordering { error_response(error::code::no_leader, "This node does not lead the tables."), {}, 0 };
		}

		return ordering { forwarding.forward(lead.node, request), {}, 0 };
	}

	return ordering { std::nullopt, nodes.peers(), lead.term };
}

// A schema operation is stamped by the node that orders it, exactly as a write to a record is, and
// the stamp travels to every node so that one create is one version everywhere. A node it was
// carried to applies what it was given; a node ordering it takes the next count of its own.
record::version router::router::schema_stamp(const request &request, const ordering &order) const
{
	if (order.carried)
	{
		return record::version { request.term, request.count };
	}

	return record::version { order.term, repository.next_count() };
}

router::response router::router::create_table(const request &request, const std::string &name)
{
	std::optional<boost::json::object> body = parse_body(request.body);

	if (!body)
	{
		return error_response(error::code::invalid_body, "The body of a table is a JSON object.");
	}

	ordering order = order_schema(request);

	if (order.answer)
	{
		return *order.answer;
	}

	// The tables are read, compared and written under the one lock every schema operation takes,
	// so what this create is valid against is what the cluster held when it was carried out and
	// not what it held when it arrived.
	std::lock_guard<std::mutex> ordered(schema_lock);

	table::table table = table::parse_table(name, *body, table_names());

	if (!table.is_valid)
	{
		return error_response(table.code, table.message);
	}

	table::table existing = repository.read_table(name);
	record::version stamp = schema_stamp(request, order);
	response created;

	if (existing.is_valid)
	{
		if (!(existing == table))
		{
			return error_response(
				error::code::table_exists,
				"A table named \"" + name + "\" exists with different options.");
		}

		created = json_response(boost::beast::http::status::ok, table.json);
	}
	else
	{
		repository.create_table(table, stamp);

		created = json_response(boost::beast::http::status::created, table.json);
	}

	std::optional<response> failure =
		cluster::refusal(forwarding.forward_all(order.peers, carried(request, order.term, stamp.count)));

	return failure ? *failure : created;
}

router::response router::router::delete_table(const request &request, const std::string &name)
{
	ordering order = order_schema(request);

	if (order.answer)
	{
		return *order.answer;
	}

	std::lock_guard<std::mutex> ordered(schema_lock);

	record::version stamp = schema_stamp(request, order);

	if (!repository.has_table(name))
	{
		// A node the delete was carried to writes the tombstone anyway. It may be a node that
		// missed the create, and the name being gone as of this version is the answer it needs
		// most — otherwise it takes the table back from a peer on the next pass.
		if (order.carried)
		{
			repository.delete_table(name, stamp);

			return empty_response(boost::beast::http::status::no_content);
		}

		if (is_incomplete())
		{
			return node_incomplete();
		}

		std::optional<response> refused =
			cluster::refusal(forwarding.forward_all(order.peers, carried(request, order.term, stamp.count)));

		return refused ? *refused : table_not_found(name);
	}

	repository.delete_table(name, stamp);

	std::optional<response> failure =
		cluster::refusal(forwarding.forward_all(order.peers, carried(request, order.term, stamp.count)));

	return failure ? *failure : empty_response(boost::beast::http::status::no_content);
}

boost::asio::awaitable<router::response> router::router::scan_records(const request &request, const std::string &name)
{
	if (!request.forwarded && nodes.is_alone())
	{
		return answered(node_alone());
	}

	if (!repository.has_table(name))
	{
		return answered(table_not_found(name));
	}

	// **The partition is what routes a scan, and it is read before anything else in the range.**
	// The rest is read by the node that answers, because the cursor is that node's own: one read
	// here would refuse a cursor issued by the node this scan is on its way to.
	std::optional<size_t> named = scan::read_partition(request.query);

	if (!named)
	{
		scan::range refused = scan::parse_range(request.query, repository.instance());

		return answered(error_response(refused.code, refused.message));
	}

	// **A scan is answered by one node, because a partition is held by one node of every zone.**
	// This node when it holds a copy, and otherwise the nearest zone's, passing over a node that
	// does not answer — the hops a read of a key takes. There is nothing to merge: every record of
	// the partition is in the one answer, in the one order the store holds them in.
	cluster::placement where = request.forwarded ? cluster::placement() : nodes.copies_of(*named);
	bool whole = !is_short_of(*named);

	if (where.local && whole)
	{
		return answered(answer_page(request, name));
	}

	// A node holding less than it owns cannot answer for a partition it may not have filled: what
	// it would send is a page short of records it owns, which no client could tell from the whole
	// of them. So it asks a copy that can, and answers for itself only when there is no other.
	if (!where.nodes.empty())
	{
		return read_record(request, where);
	}

	return answered(whole ? answer_page(request, name) : node_incomplete());
}

// The page this node's own store answers with. The range is read here and not before the scan was
// routed, so the cursor in it is one this node issued.
router::response router::router::answer_page(const request &request, const std::string &name)
{
	scan::range range = scan::parse_range(request.query, repository.instance());

	if (!range.is_valid)
	{
		return error_response(range.code, range.message);
	}

	return page_response(repository.scan_records(name, range), range, repository.instance());
}

std::set<std::string> router::router::table_names() const
{
	const std::set<table::table> tables = repository.list_tables();
	std::set<std::string> names;

	for (const auto &declared : tables)
	{
		names.insert(declared.name);
	}

	return names;
}
