#ifndef REBUILD_REBUILD_H
#define REBUILD_REBUILD_H

#include <cstddef>

#include "cluster/cluster.h"
#include "repository/repository.h"

namespace rebuild
{
	// How long a rebuild is given before it gives up and lets the node start on what it has. The
	// node is not in the membership while this runs, so a rebuild that never ends is a node that
	// never registers. Every round trip inside is bounded already; this is the bound on all of
	// them together. A node that gives up part way is thin rather than empty and will not try
	// again — an empty store is the only trigger.
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
