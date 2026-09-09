#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "log.h"
#include "cluster/partition.h"
#include "progress/patience.h"
#include "table/table.h"
#include "transfer/transfer.h"
#include "rebuild.h"

namespace
{
	// Nothing stops a rebuild but its own patience: the node is not in the membership yet, so
	// there is no pass to shut down and nothing waiting on it to finish.
	const std::atomic<bool> running(true);

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
		progress::patience &waiting)
	{
		for (size_t i = 0; i < zone.size(); i++)
		{
			if (waiting.spent())
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

			waiting.renew();

			return tables;
		}

		return std::nullopt;
	}

	restored copy_table(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::string &node,
		const std::string &name,
		const cluster::partition_set &partitions,
		size_t workers,
		progress::patience &waiting)
	{
		restored taken;
		transfer::share wanted;

		wanted.node = node;
		wanted.table = name;
		wanted.partitions = partitions;
		wanted.workers = workers;

		// The count is added to from every worker at once, so it is an atomic rather than a field
		// the last of them happens to have written.
		std::atomic<size_t> records = 0;

		taken.whole = transfer::walk(
			nodes,
			wanted,
			running,
			waiting,
			[&](const std::string &file) { records += repository.import_records(name, file); }).whole;

		taken.records = records;

		return taken;
	}

	restored from_zone(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::vector<std::string> &zone,
		const cluster::partition_set &partitions,
		size_t workers,
		progress::patience &waiting)
	{
		restored taken;
		std::optional<std::vector<table::table>> tables = read_tables(nodes, zone, waiting);

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
				restored copied = copy_table(repository, nodes, zone[j], name, partitions, workers, waiting);

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
	long seconds,
	size_t workers)
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

	progress::patience waiting(seconds);

	// Read once, because it is what every file of every table of every zone is asked for and the
	// membership this node is rebuilding against is one moment of it.
	cluster::partition_set partitions = nodes.holdings();

	for (size_t i = 1; i < zones.size(); i++)
	{
		if (zones[i].empty())
		{
			continue;
		}

		if (waiting.spent())
		{
			DEBUG("A rebuild was answered nothing for long enough to stop, so this node starts with what it has.");

			return 0;
		}

		restored taken = from_zone(repository, nodes, zones[i], partitions, workers, waiting);

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
