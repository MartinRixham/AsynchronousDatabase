#ifndef RECONCILE_RECONCILE_H
#define RECONCILE_RECONCILE_H

#include <cstddef>

#include "cluster/cluster.h"
#include "repository/repository.h"

namespace reconcile
{
	// How many keys a page of a reconcile walks of this node's own store, which is the half that
	// gives records up. The half that takes them asks for a file and not for a page.
	constexpr size_t default_page = 100;

	// How long one pass is given, half of it to each half of the pass. It is short because a node
	// being shut down waits for the pass in flight before it goes, and because what a pass does not
	// finish the pass after it starts again on: there is nothing a long pass does that two short
	// ones do not.
	constexpr long default_seconds = 20;

	// What a pass did, and whether anything is waiting on another node.
	struct outcome
	{
		size_t fetched = 0;

		size_t cleared = 0;

		size_t deferred = 0;

		bool finished = false;

		bool settled() const;
	};

	// **Neither half is safe without the other.** Clearing down alone is a shrink that loses
	// records rather than staling them; fetching alone is a store that only grows and a stale
	// value waiting for the next membership change. So one pass does both, and the clear down is
	// gated on the fetch having happened somewhere else.
	//
	// It is not a repair of a lost copy and does not try to be: what it moves is what some node
	// still has. A record every zone's owner lost at once is gone, and there is no read repair, no
	// anti-entropy and no backup to put it back.
	//
	// The one thing it can get wrong is a write ordered against a membership that has already
	// moved, which lands on a node that is no longer a copy and is cleared down in favour of what
	// the new owner has. The window is the leader's own refresh, and without this pass that write
	// would have been invisible on the node it landed on for as long as the node lived.
	outcome reconcile(
		repository::repository &repository,
		const cluster::cluster &nodes,
		size_t page = default_page,
		long seconds = default_seconds);
}

#endif
