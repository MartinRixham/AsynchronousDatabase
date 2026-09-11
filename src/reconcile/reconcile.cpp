#include <atomic>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "log.h"
#include "cluster/partition.h"
#include "cluster/placements.h"
#include "progress/patience.h"
#include "scan/scan.h"
#include "table/table.h"
#include "transfer/transfer.h"
#include "reconcile.h"

namespace
{
	// What this node now owns and holds nothing for, taken from the node that held it. A key this
	// store already has is a key the file is not applied for, which is decided by the store and not
	// here: what is here was written after the ownership moved, and what is there was written
	// before it.
	transfer::share share_of(
		const std::string &node,
		const std::string &name,
		const cluster::partition_set &partitions,
		bool values,
		size_t workers)
	{
		transfer::share wanted;

		wanted.node = node;
		wanted.table = name;
		wanted.partitions = partitions;
		wanted.values = values;
		wanted.workers = workers;

		return wanted;
	}

	reconcile::outcome fetch_from(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::string &node,
		const std::string &name,
		const cluster::partition_set &partitions,
		size_t workers,
		const std::atomic<bool> &running,
		progress::patience &waiting)
	{
		reconcile::outcome taken;

		// Added to from every worker at once, so it is an atomic rather than a field the last of
		// them happens to have written.
		std::atomic<size_t> fetched = 0;

		// A node that stopped answering is nothing more this pass can do about that node, so the
		// walk of it is finished — and what it holds of this share is still unknown, which is what
		// `refused` carries to the pass that asks it again. A walk its own patience ended is
		// neither: it is a pass with more of this share still to read.
		transfer::outcome done = transfer::walk(
			nodes,
			share_of(node, name, partitions, true, workers),
			running,
			waiting,
			[&](const std::string &file) { fetched += repository.import_records(name, file); });

		taken.finished = done.whole || done.refused;
		taken.refused = done.refused;
		taken.fetched = fetched;

		return taken;
	}

	// What a walk of this node's own store found that does not belong here: how many records, and
	// which partitions each node of this node's own zone would have to be asked about for them.
	struct misplaced
	{
		size_t records = 0;

		// Records this node holds that no node owns, which is nothing a pass can act on.
		size_t orphaned = 0;

		bool finished = false;

		std::map<std::string, cluster::partition_set> elsewhere;
	};

	// The keys alone. What is being decided is where a record belongs and not what is in it, and a
	// page of values is sixteen megabytes a record of answer to that.
	misplaced walk_table(
		const repository::repository &repository,
		const cluster::cluster &nodes,
		const std::string &name,
		size_t page,
		const std::atomic<bool> &running,
		progress::patience &waiting)
	{
		misplaced found;
		scan::range range;

		range.is_valid = true;
		range.limit = page;
		range.values = false;

		cluster::placements placed(nodes);

		while (running && !waiting.spent())
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
					found.orphaned++;

					continue;
				}

				// The copy in this node's own zone is the first replicas() names, and it is the
				// only one worth asking: the record here is this zone's copy of the key, so
				// another zone still having one says nothing about whether this may go.
				found.elsewhere[where.nodes.front()].set(cluster::partition_of(key));
				found.records++;
			}

			if (!walked.has_more || !has_last || !read_any)
			{
				found.finished = true;

				return found;
			}

			range.from = last;
			range.has_from = true;

			waiting.renew();
		}

		return found;
	}

	// The tables a node of this cluster has that this one does not. **A pass creates them and never
	// drops one**: a table here that no peer named is a delete this node missed or a peer that is
	// wrong about the schema, and only one of those is worth acting on — where a table this node
	// is missing is every write to it refused for the keys this node owns.
	size_t declare_tables(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::vector<std::vector<std::string>> &zones,
		progress::patience &waiting)
	{
		for (size_t zone = 0; zone < zones.size(); zone++)
		{
			std::optional<std::vector<table::table>> named = transfer::tables(nodes, zones[zone], waiting);

			if (!named)
			{
				continue;
			}

			size_t declared = 0;

			for (size_t i = 0; i < named->size(); i++)
			{
				if (!repository.has_table((*named)[i].name))
				{
					repository.create_table((*named)[i]);

					declared++;
				}
			}

			return declared;
		}

		return 0;
	}

	// **What makes the delete safe is that the owner said what it holds.** A key is given up only
	// when the node that owns it in this node's own zone answers with that key in a file of its
	// own, which is the same promise a record at a time made and one question for a share of them.
	reconcile::outcome clear_table(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::string &name,
		size_t page,
		size_t workers,
		const std::atomic<bool> &running,
		progress::patience &waiting)
	{
		misplaced found = walk_table(repository, nodes, name, page, running, waiting);
		reconcile::outcome given;
		std::atomic<size_t> cleared = 0;

		given.finished = found.finished;

		for (std::map<std::string, cluster::partition_set>::const_iterator it = found.elsewhere.begin();
			it != found.elsewhere.end();
			++it)
		{
			// The keys alone: what is being asked is which of them the owner has, and the values
			// are already here.
			transfer::outcome walked = transfer::walk(
				nodes,
				share_of(it->first, name, it->second, false, workers),
				running,
				waiting,
				[&](const std::string &file) { cleared += repository.clear_records(name, file); });

			if (!walked.whole && !walked.refused)
			{
				given.finished = false;
			}
		}

		given.cleared = cleared;

		// What is left is what the owner has not taken over yet, which is a copy this node keeps
		// and asks about again. A write that landed here between the walk and the file is why this
		// cannot simply be subtracted the other way round.
		given.deferred = found.orphaned + (found.records > given.cleared ? found.records - given.cleared : 0);

		return given;
	}
}

bool reconcile::outcome::settled() const
{
	return finished && !refused && deferred == 0;
}

bool reconcile::outcome::moved() const
{
	return fetched > 0 || cleared > 0;
}

reconcile::outcome reconcile::reconcile(
	repository::repository &repository,
	const cluster::cluster &nodes,
	const std::atomic<bool> &running,
	size_t page,
	long seconds,
	size_t workers)
{
	outcome done;

	std::vector<std::vector<std::string>> zones = nodes.zones();

	if (zones.empty())
	{
		done.finished = true;

		return done;
	}

	// A half apiece, and each of them is patience rather than a deadline: a half still being sent
	// records is a half to leave alone, because the pass after this one would start it again from
	// the beginning.
	progress::patience fetching(seconds);

	// Before the tables are read, so that a table this pass declares is a table this pass also
	// fills: nothing else in the cluster puts one here. The rebuild that copies them runs on an
	// empty store alone, so a node that missed a create while it was out of the membership has no
	// other way back to the schema the rest of the cluster is on.
	size_t declared = running ? declare_tables(repository, nodes, zones, fetching) : 0;

	if (declared > 0)
	{
		DEBUG("Declared " + std::to_string(declared) + " tables the rest of the cluster has.");
	}

	std::set<table::table> tables = repository.list_tables();

	// Read once, because every file of every table of every node is asked for against it and the
	// membership this pass is acting on is one moment of it.
	cluster::partition_set partitions = nodes.holdings();

	// A half that has run out of patience is what a walk it is given answers, so the loops carry no
	// clock of their own: what is left of them is asked and does nothing.
	bool finished = true;

	// A share this pass was refused is one it knows nothing about, where a share it walked and
	// deferred is one it knows the owner has not taken over yet. Both leave the pass unsettled.
	bool refused = false;

	for (std::set<table::table>::const_iterator it = tables.begin(); it != tables.end(); ++it)
	{
		for (size_t zone = 0; zone < zones.size(); zone++)
		{
			for (size_t node = 0; node < zones[zone].size(); node++)
			{
				outcome taken = fetch_from(
					repository, nodes, zones[zone][node], it->name, partitions, workers, running, fetching);

				done.fetched += taken.fetched;

				if (!taken.finished)
				{
					finished = false;
				}

				if (taken.refused)
				{
					refused = true;
				}
			}
		}
	}

	progress::patience clearing(seconds);

	for (std::set<table::table>::const_iterator it = tables.begin(); it != tables.end(); ++it)
	{
		outcome given = clear_table(repository, nodes, it->name, page, workers, running, clearing);

		done.cleared += given.cleared;
		done.deferred += given.deferred;

		if (!given.finished)
		{
			finished = false;
		}
	}

	done.finished = finished;
	done.refused = refused;

	if (done.fetched > 0 || done.cleared > 0 || done.deferred > 0)
	{
		DEBUG(
			"Reconciled " + std::to_string(done.fetched) + " records fetched, " +
			std::to_string(done.cleared) + " cleared, " + std::to_string(done.deferred) + " deferred.");
	}

	return done;
}
