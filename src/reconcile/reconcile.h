#ifndef RECONCILE_RECONCILE_H
#define RECONCILE_RECONCILE_H

#include <atomic>
#include <cstddef>

#include "cluster/cluster.h"
#include "repository/repository.h"
#include "transfer/transfer.h"

namespace reconcile
{
	// How many keys a page of a reconcile walks of this node's own store, which is the half that
	// gives records up. The half that takes them asks for a file and not for a page.
	constexpr size_t default_page = 100;

	// How long each half of a pass goes on getting nowhere before it stops. **It is not a bound on
	// the whole of a pass**: a pass that ends while records are still moving is one the pass after
	// it starts over from the beginning, so a share that takes ten passes is nine of them re-reading
	// what the ones before them already took. What ends a pass is having run out of things to move.
	//
	// A node being shut down still waits for the pass in flight, so a pass takes the flag that says
	// whether it should still be running rather than being kept short enough not to matter.
	constexpr long default_seconds = 20;

	// What a pass did, and whether anything is waiting on another node.
	struct outcome
	{
		size_t fetched = 0;

		size_t cleared = 0;

		size_t deferred = 0;

		bool finished = false;

		// Whether a node this pass asked for a share would not answer. What that node holds for
		// this node is unknown rather than absent, so the membership this pass ran against has
		// not been applied — and a membership that does not move again buys no pass to apply it.
		bool refused = false;

		bool settled() const;

		// Whether the pass moved anything, which is what says it is worth running another.
		bool moved() const;
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
		const std::atomic<bool> &running,
		size_t page = default_page,
		long seconds = default_seconds,
		size_t workers = transfer::default_workers);
}

#endif
