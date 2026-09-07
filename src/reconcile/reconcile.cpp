#include <chrono>
#include <set>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "log.h"
#include "record/record.h"
#include "scan/scan.h"
#include "table/table.h"
#include "url/url.h"
#include "reconcile.h"

namespace
{
	// Everything the cluster sends carries the forwarded header, so a scan asked this way is that
	// node's own share rather than its whole zone's merged answer, and a HEAD asked this way is
	// what that node holds rather than what its zone can find.
	router::request share_request(const std::string &table, const std::string &from, bool has_from, size_t page)
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

	// A HEAD rather than a GET: what is being asked is whether the node holds the record, and the
	// value may be sixteen megabytes of answer to a question about its existence.
	router::request holds_request(const std::string &table, const std::string &key)
	{
		router::request request;

		request.method = boost::beast::http::verb::head;
		request.path = std::vector<std::string> { "table", table, "key", key };

		return request;
	}

	bool out_of_time(const std::chrono::steady_clock::time_point &deadline)
	{
		return std::chrono::steady_clock::now() >= deadline;
	}

	std::string field(const boost::json::object &object, const std::string &name)
	{
		if (!object.contains(name) || !object.at(name).is_string())
		{
			return "";
		}

		return std::string(object.at(name).as_string());
	}

	// One node's share of one table, a page at a time, keeping what this node owns and holds
	// nothing for. Paging is by bound and not by cursor: a cursor names the instance that issued
	// it, where a key is a position any node will take.
	//
	// **What is already here is never overwritten.** A local record is this node's own copy and as
	// current as any: a write reaches every copy, so another node's answer is the same value or an
	// older one, and writing it back would be undoing a write rather than filling in a gap.
	void fetch_from(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::string &node,
		const std::string &name,
		size_t page,
		const std::chrono::steady_clock::time_point &deadline,
		size_t *fetched)
	{
		std::string from;
		bool has_from = false;

		while (!out_of_time(deadline))
		{
			router::response answer = nodes.send(node, share_request(name, from, has_from, page));

			if (answer.status != boost::beast::http::status::ok ||
				!answer.json.contains("records") || !answer.json.at("records").is_array())
			{
				DEBUG("Node " + node + " did not answer a scan of \"" + name + "\" for a reconcile.");

				return;
			}

			const boost::json::array &records = answer.json.at("records").as_array();
			std::string last;
			bool has_last = false;
			bool read_any = false;

			for (size_t i = 0; i < records.size(); i++)
			{
				if (!records[i].is_object())
				{
					continue;
				}

				const boost::json::object &object = records[i].as_object();
				std::string key = field(object, "key");

				if (key.empty())
				{
					continue;
				}

				last = key;
				has_last = true;

				// A bound is inclusive, so every page after the first begins again with the key it
				// resumed at.
				if (has_from && key == from)
				{
					continue;
				}

				read_any = true;

				if (nodes.replicas(key).local && !repository.read_record(name, key))
				{
					repository.write_record(name, record::valid_record(key, field(object, "value")));

					(*fetched)++;
				}
			}

			// No cursor is a range that is exhausted.
			if (!answer.json.contains("next"))
			{
				return;
			}

			// A page that carried nothing to resume from, or nothing but the key it resumed at, is
			// a page that asking again would ask for for ever.
			if (!has_last || !read_any)
			{
				DEBUG("A scan of \"" + name + "\" on " + node + " made no progress, so the fetch stops.");

				return;
			}

			from = last;
			has_from = true;
		}
	}

	// Whether the node that owns the key in *this node's own zone* holds it.
	//
	// It has to be that node and not any copy. The record here is this zone's copy of it, so
	// deleting it because another zone still has one is a zone left holding nothing — which is the
	// copy this cluster keeps in every zone, thrown away. `replicas` puts the copy in this node's
	// own zone first, and a key this node does not own has exactly one owner in its zone.
	bool owner_holds(
		const cluster::cluster &nodes,
		const std::string &name,
		const std::string &key,
		const std::vector<std::string> &owners)
	{
		router::response answer = nodes.send(owners.front(), holds_request(name, key));

		return answer.status == boost::beast::http::status::ok;
	}

	// The local store, a page at a time, giving up the records this node no longer owns.
	void clear_table(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::string &name,
		size_t page,
		const std::chrono::steady_clock::time_point &deadline,
		reconcile::outcome *done)
	{
		scan::range range;

		range.is_valid = true;
		range.limit = page;

		// The keys alone. What is being decided is where a record belongs and not what is in it,
		// and a page of values is sixteen megabytes a record of answer to that.
		range.values = false;

		while (!out_of_time(deadline))
		{
			scan::page walked = repository.scan_records(name, range);
			std::string last;
			bool has_last = false;
			bool read_any = false;

			for (size_t i = 0; i < walked.records.size(); i++)
			{
				const std::string &key = walked.records[i].key;

				last = key;
				has_last = true;

				if (range.has_from && key == range.from)
				{
					continue;
				}

				read_any = true;

				cluster::placement where = nodes.replicas(key);

				if (where.local)
				{
					continue;
				}

				// A key this node holds no copy of and owns no copy of is one nothing can be
				// asked about, which is an instance standing alone rather than a key to delete.
				if (where.nodes.empty())
				{
					done->deferred++;

					continue;
				}

				if (owner_holds(nodes, name, key, where.nodes))
				{
					repository.delete_record(name, key);

					done->cleared++;
				}
				else
				{
					done->deferred++;
				}
			}

			if (!walked.has_more || !has_last || !read_any)
			{
				return;
			}

			range.from = last;
			range.has_from = true;
		}
	}
}

bool reconcile::outcome::settled() const
{
	return deferred == 0;
}

reconcile::outcome reconcile::reconcile(
	repository::repository &repository,
	const cluster::cluster &nodes,
	size_t page,
	long seconds)
{
	outcome done;

	// The zones without this node, this node's own first. An instance standing alone is named none
	// of them, owns every key and has nobody to fetch from: both halves are nothing at all.
	std::vector<std::vector<std::string>> zones = nodes.zones();

	if (zones.empty())
	{
		return done;
	}

	std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

	std::set<table::table> tables = repository.list_tables();

	// Fetching first, so that a node which gained a partition holds it before the node that lost
	// it asks whether it does. Both orders converge — a clear down that is refused is deferred and
	// asked again on the next pass — but this one converges in a single pass around the cluster.
	for (std::set<table::table>::const_iterator it = tables.begin(); it != tables.end(); ++it)
	{
		// Every node of every zone, and not the first zone that answers whole. What this node is
		// missing may be on the node in its own zone that used to own it, or in one zone only —
		// a record written while another zone could not be reached is exactly that — so a pass
		// that stopped at the first zone would leave a copy short and call it settled.
		for (size_t zone = 0; zone < zones.size() && !out_of_time(deadline); zone++)
		{
			for (size_t node = 0; node < zones[zone].size() && !out_of_time(deadline); node++)
			{
				fetch_from(repository, nodes, zones[zone][node], it->name, page, deadline, &done.fetched);
			}
		}
	}

	for (std::set<table::table>::const_iterator it = tables.begin(); it != tables.end(); ++it)
	{
		clear_table(repository, nodes, it->name, page, deadline, &done);
	}

	if (done.fetched > 0 || done.cleared > 0 || done.deferred > 0)
	{
		DEBUG(
			"Reconciled " + std::to_string(done.fetched) + " records fetched, " +
			std::to_string(done.cleared) + " cleared, " + std::to_string(done.deferred) + " deferred.");
	}

	return done;
}
