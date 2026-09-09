#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "log.h"
#include "cluster/partition.h"
#include "table/table.h"
#include "url/url.h"
#include "rebuild.h"

namespace
{
	// One file of the records another node holds that belong to this one. The partitions are this
	// node's own, so what comes back is decided by the membership this node read rather than by
	// the one the node answering happens to hold.
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

	// What a rebuild took from one node or from one zone: whether the whole of it was read, and how
	// many of its records this node now holds.
	struct restored
	{
		bool whole = false;

		size_t records = 0;
	};

	std::optional<std::vector<table::table>> read_tables(
		const cluster::cluster &nodes,
		const std::vector<std::string> &zone,
		const std::chrono::steady_clock::time_point &deadline)
	{
		for (size_t i = 0; i < zone.size(); i++)
		{
			if (out_of_time(deadline))
			{
				return std::nullopt;
			}

			router::response answer = nodes.send(zone[i], table_request());

			if (answer.status != boost::beast::http::status::ok ||
				!answer.json.contains("tables") || !answer.json.at("tables").is_array())
			{
				DEBUG("Node " + zone[i] + " did not name its tables for a rebuild.");

				continue;
			}

			const boost::json::array &listed = answer.json.at("tables").as_array();
			std::vector<table::table> tables;

			for (size_t j = 0; j < listed.size(); j++)
			{
				if (!listed[j].is_object() || field(listed[j].as_object(), "name").empty())
				{
					continue;
				}

				tables.push_back(table::to_table(boost::json::serialize(listed[j])));
			}

			return tables;
		}

		return std::nullopt;
	}

	restored copy_table(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::string &node,
		const std::string &name,
		const std::string &partitions,
		const std::chrono::steady_clock::time_point &deadline)
	{
		restored taken;
		std::string from;
		bool has_from = false;

		while (true)
		{
			// The file boundary is the only place a rebuild can be given up: what is written
			// already is this node's own, and the file not asked for is all that is lost.
			if (out_of_time(deadline))
			{
				DEBUG("A file of \"" + name + "\" from " + node + " ran out of time for a rebuild.");

				return taken;
			}

			router::response answer = nodes.send(node, file_request(name, partitions, from, has_from));

			if (answer.status != boost::beast::http::status::ok)
			{
				DEBUG("Node " + node + " did not answer for a file of \"" + name + "\" for a rebuild.");

				return taken;
			}

			taken.records += repository.import_records(name, answer.text);

			// Nowhere to resume is a walk that reached the end of the table.
			if (answer.file.next.empty())
			{
				taken.whole = true;

				return taken;
			}

			// A file that resumes where the one before it did is a walk that would ask for ever.
			if (has_from && answer.file.next == from)
			{
				DEBUG("A file of \"" + name + "\" from " + node + " made no progress, so the rebuild stops.");

				return taken;
			}

			from = answer.file.next;
			has_from = true;
		}
	}

	restored from_zone(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::vector<std::string> &zone,
		const std::string &partitions,
		const std::chrono::steady_clock::time_point &deadline)
	{
		restored taken;
		std::optional<std::vector<table::table>> tables = read_tables(nodes, zone, deadline);

		if (!tables)
		{
			return taken;
		}

		for (size_t i = 0; i < tables->size(); i++)
		{
			repository.create_table((*tables)[i]);
		}

		for (size_t i = 0; i < tables->size(); i++)
		{
			const std::string &name = (*tables)[i].name;

			for (size_t j = 0; j < zone.size(); j++)
			{
				restored copied = copy_table(repository, nodes, zone[j], name, partitions, deadline);

				taken.records += copied.records;

				if (!copied.whole)
				{
					return taken;
				}
			}
		}

		taken.whole = true;

		return taken;
	}
}

size_t rebuild::rebuild(
	repository::repository &repository,
	const cluster::cluster &nodes,
	long seconds)
{
	if (!repository.list_tables().empty())
	{
		return 0;
	}

	std::vector<std::vector<std::string>> zones = nodes.zones();

	if (zones.size() < 2)
	{
		return 0;
	}

	std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

	// Read once, because it is what every file of every table of every zone is asked for and the
	// membership this node is rebuilding against is one moment of it.
	std::string partitions = cluster::encode_partitions(nodes.holdings());

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

		restored taken = from_zone(repository, nodes, zones[i], partitions, deadline);

		if (taken.whole)
		{
			DEBUG("Rebuilt " + std::to_string(taken.records) + " records before joining.");

			return taken.records;
		}

		DEBUG("A node of a zone did not answer, so the rebuild asks the next zone.");
	}

	DEBUG("No zone answered a rebuild, so this node starts with what it has.");

	return 0;
}
