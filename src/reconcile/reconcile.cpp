#include <atomic>
#include <map>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

#include "log.h"
#include "cluster/partition.h"
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
		cluster::cluster &nodes,
		const std::string &node,
		const std::string &name,
		const cluster::partition_set &partitions,
		size_t workers,
		const std::stop_token &token,
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
			token,
			waiting,
			[&](const std::string &file)
			{
				fetched += repository.import_records(name, file);
				nodes.restore_terms(repository.read_terms());
			});

		taken.finished = done.whole || done.refused;
		taken.refused = done.refused;
		taken.fetched = fetched;

		if (done.whole)
		{
			taken.filled = partitions;
		}

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
	//
	// **What it walks is the partitions this node does not own**, which is where a record that
	// does not belong here can be. The store sorts the partitions apart, so the ones this node
	// owns — the whole of the table but for what has just moved — are passed over rather than read
	// through to find nothing in them.
	bool walk_partition(
		const repository::repository &repository,
		const cluster::placement &where,
		const std::string &name,
		size_t partition,
		size_t page,
		const std::stop_token &token,
		progress::patience &waiting,
		misplaced *found)
	{
		scan::range range;

		range.is_valid = true;
		range.partition = partition;
		range.limit = page;
		range.values = false;

		while (!token.stop_requested() && !waiting.spent())
		{
			scan::page walked = repository.scan_records(name, range);
			std::string last;
			bool has_last = false;
			bool read_any = false;

			for (const auto &walked_record : walked.records)
			{
				const std::string &key = walked_record.key;

				last = key;
				has_last = true;

				if (range.has_from && key == range.from)
				{
					continue;
				}

				read_any = true;

				// Records this node holds that no node owns, which is nothing a pass can act on.
				if (where.nodes.empty())
				{
					found->orphaned++;

					continue;
				}

				// The copy in this node's own zone is the first copies_of() names, and it is the
				// only one worth asking: the record here is this zone's copy of the key, so
				// another zone still having one says nothing about whether this may go.
				found->elsewhere[where.nodes.front()].set(partition);
				found->records++;
			}

			if (!walked.has_more || !has_last || !read_any)
			{
				return true;
			}

			range.from = last;
			range.has_from = true;

			waiting.renew();
		}

		return false;
	}

	misplaced walk_table(
		const repository::repository &repository,
		const cluster::cluster &nodes,
		const std::string &name,
		size_t page,
		const std::stop_token &token,
		progress::patience &waiting)
	{
		misplaced found;
		cluster::partition_set held = nodes.holdings();

		for (size_t partition = 0; partition < cluster::partition_count; partition++)
		{
			if (held.test(partition))
			{
				continue;
			}

			if (!walk_partition(
				repository,
				nodes.copies_of(partition),
				name,
				partition,
				page,
				token,
				waiting,
				&found))
			{
				return found;
			}
		}

		found.finished = true;

		return found;
	}

	// This node's schema brought up to the cluster's: every name the peer holds at a later version
	// than this node does, taken. **A name is decided on its own version and never on the
	// schema's**, because a node that missed one operation and applied the next would otherwise
	// carry a version vouching for both — which is a replication log, and there is none here.
	//
	// A table this node is missing is every write to it refused for the keys this node owns, and a
	// table the cluster dropped is a name whose tombstone says so. What a live entry replacing a
	// live entry never does is drop a column family: one create carried twice is one table.
	//
	// Nothing when no zone named its tables, which is a pass that cannot say its tables are the
	// cluster's and so cannot vouch for a partition of any of them.
	std::optional<size_t> declare_tables(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::vector<std::vector<std::string>> &zones,
		progress::patience &waiting)
	{
		for (const auto &in_zone : zones)
		{
			std::optional<table::schema> named = transfer::tables(nodes, in_zone, waiting);

			if (!named)
			{
				continue;
			}

			return repository.merge_schema(*named);
		}

		return std::nullopt;
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
		const std::stop_token &token,
		progress::patience &waiting)
	{
		misplaced found = walk_table(repository, nodes, name, page, token, waiting);
		reconcile::outcome given;
		std::atomic<size_t> cleared = 0;

		given.finished = found.finished;

		for (const auto &[node, partitions] : found.elsewhere)
		{
			// The keys alone: what is being asked is which of them the owner has, and the values
			// are already here.
			transfer::outcome walked = transfer::walk(
				nodes,
				share_of(node, name, partitions, false, workers),
				token,
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
	cluster::cluster &nodes,
	const std::stop_token &token,
	size_t page,
	long seconds,
	size_t workers)
{
	outcome done;

	// Before anything is read of what this node holds, so that a partition it gains while the pass
	// runs is one the pass does not vouch for.
	done.generation = nodes.generation();

	std::vector<std::vector<std::string>> zones = nodes.zones();

	if (zones.empty())
	{
		done.finished = true;
		done.filled.set();

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
	std::optional<size_t> declared =
		!token.stop_requested() ? declare_tables(repository, nodes, zones, fetching) : std::optional<size_t>();

	if (declared && *declared > 0)
	{
		DEBUG("Took " + std::to_string(*declared) + " names from the schema the rest of the cluster is on.");
	}

	std::set<table::table> tables = repository.list_tables();

	// The nodes holding what this node owns and which partitions of it to ask each of them for.
	// Read once, because every file of every table is asked for against it and the membership this
	// pass is acting on is one moment of it.
	//
	// **A pass asks the nodes that hold a share and not every node there is**: one node of a zone
	// holds that zone's copy of a partition, and in this node's own zone it is the node this one
	// took the partition from. A node it does not ask because the records moved on inside another
	// zone while this pass ran is a node the next pass asks, which is the same thing the clear
	// down is answered a file without a key in it by.
	cluster::partition_set held = nodes.holdings();
	std::map<std::string, cluster::partition_set> holders = nodes.holders(held);

	// A partition is whole only when every node asked for it answered for all of it, in every table.
	cluster::partition_set unfilled;

	// A half that has run out of patience is what a walk it is given answers, so the loops carry no
	// clock of their own: what is left of them is asked and does nothing.
	bool finished = true;

	// A share this pass was refused is one it knows nothing about, where a share it walked and
	// deferred is one it knows the owner has not taken over yet. Both leave the pass unsettled.
	bool refused = false;

	for (const auto &declaration : tables)
	{
		for (const auto &[node, partitions] : holders)
		{
			outcome taken = fetch_from(repository, nodes, node, declaration.name, partitions, workers, token, fetching);

			done.fetched += taken.fetched;
			unfilled |= partitions & ~taken.filled;

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

	if (declared)
	{
		done.filled = held & ~unfilled;
	}

	progress::patience clearing(seconds);

	for (const auto &declaration : tables)
	{
		outcome given = clear_table(repository, nodes, declaration.name, page, workers, token, clearing);

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
