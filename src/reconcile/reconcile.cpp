#include <chrono>
#include <set>
#include <string>
#include <vector>

#include "log.h"
#include "cluster/partition.h"
#include "cluster/placements.h"
#include "scan/scan.h"
#include "table/table.h"
#include "url/url.h"
#include "reconcile.h"

namespace
{
	// One file of the records another node holds that belong to this one, which is how a partition
	// whose owner moved travels: a key at a time is a round trip for every record of a share.
	router::request file_request(
		const std::string &table,
		const std::string &partitions,
		const std::string &from,
		bool has_from)
	{
		router::request request;

		request.method = boost::beast::http::verb::get;
		request.path = std::vector<std::string> { "table", table, "file" };
		request.query = "partitions=" + partitions;

		if (has_from)
		{
			request.query += "&from=" + url::encode(from);
		}

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

	// What this node now owns and holds nothing for, taken from the node that held it. A file it
	// already has a key of is a file that key is left out of, which is decided by the store and
	// not here: what is here was written before the ownership moved, and what is there was written
	// after it.
	reconcile::outcome fetch_from(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::string &node,
		const std::string &name,
		const std::string &partitions,
		const std::chrono::steady_clock::time_point &deadline)
	{
		reconcile::outcome taken;
		std::string from;
		bool has_from = false;

		while (!out_of_time(deadline))
		{
			router::response answer = nodes.send(node, file_request(name, partitions, from, has_from));

			if (answer.status != boost::beast::http::status::ok)
			{
				DEBUG("Node " + node + " did not answer for a file of \"" + name + "\" for a reconcile.");

				taken.finished = true;

				return taken;
			}

			taken.fetched += repository.import_records(name, answer.text);

			// Nowhere to resume is a walk that reached the end of the table.
			if (answer.file.next.empty())
			{
				taken.finished = true;

				return taken;
			}

			// A file that resumes where the one before it did is a walk that would ask for ever.
			if (has_from && answer.file.next == from)
			{
				DEBUG("A file of \"" + name + "\" on " + node + " made no progress, so the fetch stops.");

				taken.finished = true;

				return taken;
			}

			from = answer.file.next;
			has_from = true;
		}

		return taken;
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

	reconcile::outcome clear_table(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::string &name,
		size_t page,
		const std::chrono::steady_clock::time_point &deadline)
	{
		reconcile::outcome given;
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
					given.deferred++;

					continue;
				}

				// Whether the record may go is asked of the owner every time, and never cached:
				// what is remembered here is where the key belongs, not who holds it now.
				if (owner_holds(nodes, name, key, where.nodes))
				{
					repository.delete_record(name, key);

					given.cleared++;
				}
				else
				{
					given.deferred++;
				}
			}

			if (!walked.has_more || !has_last || !read_any)
			{
				given.finished = true;

				return given;
			}

			range.from = last;
			range.has_from = true;
		}

		return given;
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

	// Read once, because every file of every table of every node is asked for against it and the
	// membership this pass is acting on is one moment of it.
	std::string partitions = cluster::encode_partitions(nodes.holdings());

	// A half that has run out of its own time is what a walk it is given answers, so the loops
	// carry no clock of their own: what is left of them is asked and does nothing.
	bool finished = true;

	for (std::set<table::table>::const_iterator it = tables.begin(); it != tables.end(); ++it)
	{
		for (size_t zone = 0; zone < zones.size(); zone++)
		{
			for (size_t node = 0; node < zones[zone].size(); node++)
			{
				outcome taken = fetch_from(repository, nodes, zones[zone][node], it->name, partitions, fetching);

				done.fetched += taken.fetched;

				if (!taken.finished)
				{
					finished = false;
				}
			}
		}
	}

	std::chrono::steady_clock::time_point clearing = std::chrono::steady_clock::now() + half;

	for (std::set<table::table>::const_iterator it = tables.begin(); it != tables.end(); ++it)
	{
		outcome given = clear_table(repository, nodes, it->name, page, clearing);

		done.cleared += given.cleared;
		done.deferred += given.deferred;

		if (!given.finished)
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
