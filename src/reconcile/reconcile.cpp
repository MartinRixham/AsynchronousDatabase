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
	router::request share_request(const std::string &table, const std::string &from, bool has_from, size_t page)
	{
		router::request request;

		request.method = boost::beast::http::verb::get;
		request.path = std::vector<std::string> { "table", table, "key" };
		request.query = "limit=" + std::to_string(page) + "&values=false";

		if (has_from)
		{
			request.query += "&from=" + url::encode(from);
		}

		return request;
	}

	router::request record_request(const std::string &table, const std::string &key)
	{
		router::request request;

		request.method = boost::beast::http::verb::get;
		request.path = std::vector<std::string> { "table", table, "key", key };

		return request;
	}

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

	// Paging is by bound and not by cursor: a cursor names the instance that issued it, where a
	// key is a position any node will take.
	bool fetch_from(
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

		// Where a key belongs is decided by its partition, so one walk asks that 256 times rather
		// than once for every key of another node's share.
		cluster::placements where(nodes);

		while (!out_of_time(deadline))
		{
			router::response answer = nodes.send(node, share_request(name, from, has_from, page));

			if (answer.status != boost::beast::http::status::ok ||
				!answer.json.contains("records") || !answer.json.at("records").is_array())
			{
				DEBUG("Node " + node + " did not answer a scan of \"" + name + "\" for a reconcile.");

				return true;
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

				if (where.of(key).local && !repository.read_record(name, key))
				{
					// A record that went between the page and this is one there is nothing to
					// take: the node that had it is not the node that owns it.
					router::response value = nodes.send(node, record_request(name, key));

					if (value.status == boost::beast::http::status::ok)
					{
						repository.write_record(name, record::valid_record(key, value.text));

						(*fetched)++;
					}
				}
			}

			// No cursor is a range that is exhausted.
			if (!answer.json.contains("next"))
			{
				return true;
			}

			// A page that carried nothing to resume from, or nothing but the key it resumed at, is
			// a page that asking again would ask for for ever.
			if (!has_last || !read_any)
			{
				DEBUG("A scan of \"" + name + "\" on " + node + " made no progress, so the fetch stops.");

				return true;
			}

			from = last;
			has_from = true;
		}

		return false;
	}

	bool owner_holds(
		const cluster::cluster &nodes,
		const std::string &name,
		const std::string &key,
		const std::vector<std::string> &owners)
	{
		router::response answer = nodes.send(owners.front(), holds_request(name, key));

		return answer.status == boost::beast::http::status::ok;
	}

	bool clear_table(
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

		cluster::placements placed(nodes);

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

				const cluster::placement &where = placed.of(key);

				if (where.local)
				{
					continue;
				}

				if (where.nodes.empty())
				{
					done->deferred++;

					continue;
				}

				// Whether the record may go is asked of the owner every time, and never cached:
				// what is remembered here is where the key belongs, not who holds it now.
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
				return true;
			}

			range.from = last;
			range.has_from = true;
		}

		return false;
	}
}

bool reconcile::outcome::settled() const
{
	return finished && deferred == 0;
}

reconcile::outcome reconcile::reconcile(
	repository::repository &repository,
	const cluster::cluster &nodes,
	size_t page,
	long seconds)
{
	outcome done;

	std::vector<std::vector<std::string>> zones = nodes.zones();

	if (zones.empty())
	{
		done.finished = true;

		return done;
	}

	std::chrono::milliseconds half(seconds * 1000 / 2);

	std::chrono::steady_clock::time_point fetching = std::chrono::steady_clock::now() + half;

	std::set<table::table> tables = repository.list_tables();

	// A half that has run out of its own time is what a walk it is given answers, so the loops
	// carry no clock of their own: what is left of them is asked and does nothing.
	bool finished = true;

	for (std::set<table::table>::const_iterator it = tables.begin(); it != tables.end(); ++it)
	{
		for (size_t zone = 0; zone < zones.size(); zone++)
		{
			for (size_t node = 0; node < zones[zone].size(); node++)
			{
				if (!fetch_from(repository, nodes, zones[zone][node], it->name, page, fetching, &done.fetched))
				{
					finished = false;
				}
			}
		}
	}

	std::chrono::steady_clock::time_point clearing = std::chrono::steady_clock::now() + half;

	for (std::set<table::table>::const_iterator it = tables.begin(); it != tables.end(); ++it)
	{
		if (!clear_table(repository, nodes, it->name, page, clearing, &done))
		{
			finished = false;
		}
	}

	done.finished = finished;

	if (done.fetched > 0 || done.cleared > 0 || done.deferred > 0)
	{
		DEBUG(
			"Reconciled " + std::to_string(done.fetched) + " records fetched, " +
			std::to_string(done.cleared) + " cleared, " + std::to_string(done.deferred) + " deferred.");
	}

	return done;
}
