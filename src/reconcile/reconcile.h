#ifndef RECONCILE_RECONCILE_H
#define RECONCILE_RECONCILE_H

#include <cstddef>

#include "cluster/cluster.h"
#include "repository/repository.h"

namespace reconcile
{
	// How many records a page of a reconcile asks for, as in a rebuild: well under what a scan
	// would give, because a fetched page carries values and a value may be sixteen megabytes.
	constexpr size_t default_page = 100;

	// How long one pass is given, half of it to each half of the pass. It is short because a node
	// being shut down waits for the pass in flight before it goes, and because what a pass does not
	// finish the pass after it starts again on: there is nothing a long pass does that two short
	// ones do not.
	constexpr long default_seconds = 20;

	// What a pass did, and whether anything is waiting on another node.
	struct outcome
	{
		// Records this node owns and held nothing for, taken from a node that has them.
		size_t fetched = 0;

		// Records this node no longer owns, deleted because the node that owns them now has them.
		size_t cleared = 0;

		// Records this node no longer owns and has kept, because the node that owns them in this
		// zone has not fetched them yet. **This is the whole safety of clearing down**: a copy is
		// deleted only once the copy that replaces it exists, so a pass that ran on a membership
		// this node had wrong for a moment deletes nothing at all.
		size_t deferred = 0;

		// Whether the pass reached the end of what this membership asks of it, rather than its own
		// clock ending it first. **A truncated pass is never settled, however little it found to
		// do**: what it did not reach it also found nothing in, so a pass cut short in the fetch
		// half is a clear down that never ran and deferred nothing to say so.
		bool finished = false;

		// Nothing is waiting on another node and the pass got to the end of it, so a pass now
		// would do the same nothing again.
		bool settled() const;
	};

	// Moves the records whose owner changed, in both directions, and answers what it moved.
	//
	// A key belongs to one node in each zone, and which node that is falls out of the membership:
	// a node joining or leaving redraws the split inside its zone, and the records do not move
	// with it. Fetching is what a node that has *gained* a partition owes it — until then its zone
	// is a copy short, and the record is answered only by the zones that happen still to have it.
	// Clearing down is what a node that has *lost* one owes the zone: nothing reads that copy, it
	// stops taking writes the moment ownership moves, and it comes back as a stale answer if the
	// membership ever hands the partition back.
	//
	// **Neither half is safe without the other.** Clearing down alone is a shrink that loses
	// records rather than staling them; fetching alone is a store that only grows and a stale
	// value waiting for the next membership change. So one pass does both, and the clear down is
	// gated on the fetch having happened somewhere else.
	//
	// It is not a repair of a lost copy and does not try to be: what it moves is what some node
	// still has. A record every zone's owner lost at once is gone, and there is no read repair, no
	// anti-entropy and no backup to put it back.
	//
	// Nor does it choose between two values. It never overwrites what this node holds, and it gives
	// a copy up only once the node that owns it has one — so the one thing it can get wrong is a
	// write ordered against a membership that has already moved, which lands on a node that is no
	// longer a copy and is cleared down in favour of what the new owner has. The window is the
	// leader's own refresh, and without this pass that write would have been invisible on the node
	// it landed on for as long as the node lived.
	outcome reconcile(
		repository::repository &repository,
		const cluster::cluster &nodes,
		size_t page = default_page,
		long seconds = default_seconds);
}

#endif
