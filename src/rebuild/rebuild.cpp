#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "log.h"
#include "record/record.h"
#include "table/table.h"
#include "url/url.h"
#include "rebuild.h"

namespace
{
	// Everything a cluster sends carries the forwarded header, so a scan asked this way is that
	// one node's own share rather than its whole zone's merged answer — which lets a rebuild ask
	// each node of a zone once and add up what they hold.
	router::request scan_request(const std::string &table, const std::string &from, bool has_from, size_t page)
	{
		router::request request;

		request.method = boost::beast::http::verb::get;
		request.path = std::vector<std::string> { "table", table, "key" };
		request.query = "limit=" + std::to_string(page) + "&values=true";

		if (has_from)
		{
			request.query += "&from=" + url::encode(from);
		}

		return request;
	}

	// Every round trip inside is bounded by the client that makes it; a rebuild is bounded by the
	// clock instead, because it is all of them together that holds a node out of the membership.
	bool out_of_time(const std::chrono::steady_clock::time_point &deadline)
	{
		return std::chrono::steady_clock::now() >= deadline;
	}

	router::request table_request()
	{
		router::request request;

		request.method = boost::beast::http::verb::get;
		request.path = std::vector<std::string> { "table" };

		return request;
	}

	std::string field(const boost::json::object &object, const std::string &name)
	{
		if (!object.contains(name) || !object.at(name).is_string())
		{
			return "";
		}

		return std::string(object.at(name).as_string());
	}

	// Every node holds every table, so the first node of the zone to answer has answered for all
	// of them.
	bool read_tables(
		const cluster::cluster &nodes,
		const std::vector<std::string> &zone,
		const std::chrono::steady_clock::time_point &deadline,
		std::vector<table::table> *tables)
	{
		for (size_t i = 0; i < zone.size(); i++)
		{
			if (out_of_time(deadline))
			{
				return false;
			}

			router::response answer = nodes.send(zone[i], table_request());

			if (answer.status != boost::beast::http::status::ok ||
				!answer.json.contains("tables") || !answer.json.at("tables").is_array())
			{
				DEBUG("Node " + zone[i] + " did not name its tables for a rebuild.");

				continue;
			}

			const boost::json::array &listed = answer.json.at("tables").as_array();

			for (size_t j = 0; j < listed.size(); j++)
			{
				if (!listed[j].is_object() || field(listed[j].as_object(), "name").empty())
				{
					continue;
				}

				tables->push_back(table::to_table(boost::json::serialize(listed[j])));
			}

			return true;
		}

		return false;
	}

	// One node's share of one table, a page at a time. Paging is by bound and not by cursor: a
	// cursor names the instance that issued it, where a key is a position any node will take.
	bool copy_table(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::string &node,
		const std::string &name,
		size_t page,
		const std::chrono::steady_clock::time_point &deadline,
		size_t *restored)
	{
		std::string from;
		bool has_from = false;

		// Where a key belongs is decided by its partition, so one walk asks that 256 times rather
		// than once for every key of the zone being read.
		cluster::placements where(nodes);

		while (true)
		{
	// The page boundary is the only place a rebuild can be given up: what is written already
	// is this node's own, and the page not asked for is all that is lost.
			if (out_of_time(deadline))
			{
				DEBUG("A scan of \"" + name + "\" on " + node + " ran out of time for a rebuild.");

				return false;
			}

			router::response answer = nodes.send(node, scan_request(name, from, has_from, page));

			if (answer.status != boost::beast::http::status::ok ||
				!answer.json.contains("records") || !answer.json.at("records").is_array())
			{
				DEBUG("Node " + node + " did not answer a scan of \"" + name + "\" for a rebuild.");

				return false;
			}

			const boost::json::array &records = answer.json.at("records").as_array();
			std::string last;
			bool has_last = false;
			bool read_any = false;

			for (size_t i = 0; i < records.size(); i++)
			{
				if (!records[i].is_object() || !records[i].as_object().contains("key") ||
					!records[i].as_object().at("key").is_string())
				{
					continue;
				}

				const boost::json::object &object = records[i].as_object();
				std::string key = field(object, "key");

				last = key;
				has_last = true;

				// A bound is inclusive, so every page after the first begins again with the key it
				// resumed at.
				if (has_from && key == from)
				{
					continue;
				}

				read_any = true;

				// Only what this node will hold. The membership was read before the node
				// registered, and a node is a member of its own cluster whatever etcd says, so
				// the copies of a key are already the ones it will have once it joins.
				if (where.of(key).local)
				{
					repository.write_record(name, record::valid_record(key, field(object, "value")));

					(*restored)++;
				}
			}

			// No cursor is a range that is exhausted.
			if (!answer.json.contains("next"))
			{
				return true;
			}

			// A page that carried nothing to resume from, or nothing but the key it resumed at,
			// is a page that asking again would ask for for ever.
			if (!has_last || !read_any)
			{
				DEBUG("A scan of \"" + name + "\" on " + node + " made no progress, so the rebuild stops.");

				return false;
			}

			from = last;
			has_from = true;
		}
	}

	// A zone holds a copy of the whole keyspace between its nodes, so all of them have to answer
	// for what they hold between them to be the whole of it.
	bool from_zone(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::vector<std::string> &zone,
		size_t page,
		const std::chrono::steady_clock::time_point &deadline,
		size_t *restored)
	{
		std::vector<table::table> tables;

		if (!read_tables(nodes, zone, deadline, &tables))
		{
			return false;
		}

		// The schema first, because a record can only be written where its table is. Putting a
		// table back is not creating one, so nothing here has to name its dependencies in order.
		for (size_t i = 0; i < tables.size(); i++)
		{
			repository.create_table(tables[i]);
		}

		for (size_t i = 0; i < tables.size(); i++)
		{
			const std::string &name = tables[i].name;

			// All of them, and it stops at the first that does not answer: what the nodes of a
			// zone hold is one whole copy only when every one of them has said what it holds.
			bool whole = std::all_of(
				zone.begin(),
				zone.end(),
				[&](const std::string &node)
				{
					return copy_table(repository, nodes, node, name, page, deadline, restored);
				});

			if (!whole)
			{
				return false;
			}
		}

		return true;
	}
}

size_t rebuild::rebuild(
	repository::repository &repository,
	const cluster::cluster &nodes,
	size_t page,
	long seconds)
{
	// A node holding anything at all is a node that kept its store, and reading a whole zone to
	// learn that would cost the keyspace on every restart.
	if (!repository.list_tables().empty())
	{
		return 0;
	}

	// The first group is this node's own zone. What this node is missing is missing from that zone
	// as a whole, so the copy to read is in another one.
	std::vector<std::vector<std::string>> zones = nodes.zones();

	if (zones.size() < 2)
	{
		return 0;
	}

	std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

	for (size_t i = 1; i < zones.size(); i++)
	{
		if (zones[i].empty())
		{
			continue;
		}

		if (out_of_time(deadline))
		{
			DEBUG("A rebuild ran out of time, so this node starts with what it has.");

			return 0;
		}

		size_t restored = 0;

		if (from_zone(repository, nodes, zones[i], page, deadline, &restored))
		{
			DEBUG("Rebuilt " + std::to_string(restored) + " records before joining.");

			return restored;
		}

		// A zone with a node that did not answer cannot give the whole of what it holds, so the
		// whole of it is asked of the next zone. What was written already is written again with
		// the same value.
		DEBUG("A node of a zone did not answer, so the rebuild asks the next zone.");
	}

	DEBUG("No zone answered a rebuild, so this node starts with what it has.");

	return 0;
}
