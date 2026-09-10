#ifndef REBUILD_REBUILD_H
#define REBUILD_REBUILD_H

#include <cstddef>

#include "cluster/cluster.h"
#include "repository/repository.h"
#include "transfer/transfer.h"

namespace rebuild
{
	// How long a rebuild goes on being sent nothing before it gives up and lets the node start on
	// what it has. **It is not a bound on the whole of a rebuild**: how long one takes is how much
	// there is to read, so a wall clock over all of it is a clock that a large enough store always
	// runs out — and a node that runs out is thin rather than empty and will not try again, because
	// an empty store is the only trigger. A rebuild still being sent files is one to leave alone.
	//
	// The node is not in the membership while this runs, so what a slow one costs is a slow start
	// and nothing else. Every round trip inside is bounded already; this is the bound on a node
	// that has stopped answering between them.
	constexpr long default_seconds = 600;

	// What a rebuild took, and whether it read the whole of the share this node owns. A count
	// alone cannot say: a store that needed nothing and a zone that stopped answering both took
	// no records, and only one of them leaves the node holding less than it owns.
	struct outcome
	{
		size_t records = 0;

		bool whole = true;
	};

	// Nothing else in the cluster puts a lost copy back: there is no read repair, no anti-entropy,
	// no hinted handoff and no replication log.
	//
	// **It runs before the node registers in etcd**, which is what makes it free to take as long
	// as it needs: a node that is not in the membership is nobody's copy, so no read is answered
	// from it and no write waits on it.
	outcome rebuild(
		repository::repository &repository,
		const cluster::cluster &nodes,
		long seconds = default_seconds,
		size_t workers = transfer::default_workers);
}

#endif
