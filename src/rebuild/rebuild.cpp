#include <atomic>
#include <map>
#include <optional>
#include <set>
#include <stop_token>
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
	const std::stop_token unstopped;

	// What a rebuild took from one node or from one zone: whether the whole of it was read, and how
	// many of its records this node now holds.
	struct restored
	{
		bool whole = false;

		size_t records = 0;
	};

	restored copy_table(
		repository::repository &repository,
		cluster::cluster &nodes,
		const cluster::forwarder &forwarding,
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
			forwarding,
			wanted,
			unstopped,
			waiting,
			[&](const std::string &file)
			{
				records += repository.import_records(name, file);
				nodes.restore_terms(repository.read_terms());
			}).whole;

		taken.records = records;

		return taken;
	}

	restored from_zone(
		repository::repository &repository,
		cluster::cluster &nodes,
		const cluster::forwarder &forwarding,
		const std::vector<std::string> &zone,
		const cluster::partition_set &partitions,
		size_t workers,
		progress::patience &waiting)
	{
		restored taken;
		std::optional<table::schema> tables = transfer::tables(forwarding, zone, waiting);

		if (!tables)
		{
			return taken;
		}

		// The store is empty, so the merge is the whole schema taken as it stands — the names the
		// cluster has dropped included, which is what keeps this node from declaring one of them
		// again the moment a peer's answer is a moment old.
		repository.merge_schema(*tables);

		std::set<std::string> named = tables->names();

		// The nodes of this zone that hold the share, and which partitions of it to ask each of
		// them for. **A zone's copy is split between its nodes**, so a node that owns none of this
		// share would answer a file with nothing of it in it — having read its whole table to find
		// that out, which is what a share asked of every node of a zone costs.
		std::map<std::string, cluster::partition_set> holders = nodes.holders_in(partitions, zone);

		for (const std::string &name : named)
		{
			for (const auto &[holder, share] : holders)
			{
				restored copied = copy_table(repository, nodes, forwarding, holder, name, share, workers, waiting);

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

rebuild::outcome rebuild::rebuild(
	repository::repository &repository,
	cluster::cluster &nodes,
	const cluster::forwarder &forwarding,
	long seconds,
	size_t workers)
{
	if (!repository.list_tables().empty())
	{
		return outcome();
	}

	std::vector<std::vector<std::string>> zones = nodes.zones();

	// A node with nowhere to read from is not a node that came up short: it owns every key it
	// is asked about, and a miss it reports is the store and not the rebuild.
	if (zones.size() < 2)
	{
		return outcome();
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

			return outcome { 0, false };
		}

		restored taken = from_zone(repository, nodes, forwarding, zones[i], partitions, workers, waiting);

		if (taken.whole)
		{
			DEBUG("Rebuilt " + std::to_string(taken.records) + " records before joining.");

			return outcome { taken.records, true };
		}

		DEBUG("A node of a zone did not answer, so the rebuild asks the next zone.");
	}

	DEBUG("No zone answered a rebuild, so this node starts with what it has.");

	return outcome { 0, false };
}
