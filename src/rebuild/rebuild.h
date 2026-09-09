#ifndef REBUILD_REBUILD_H
#define REBUILD_REBUILD_H

#include <cstddef>

#include "cluster/cluster.h"
#include "repository/repository.h"

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

	// Nothing else in the cluster puts a lost copy back: there is no read repair, no anti-entropy,
	// no hinted handoff and no replication log.
	//
	// **It runs before the node registers in etcd**, which is what makes it free to take as long
	// as it needs: a node that is not in the membership is nobody's copy, so no read is answered
	// from it and no write waits on it.
	size_t rebuild(repository::repository &repository, const cluster::cluster &nodes, long seconds = default_seconds);
}

#endif
