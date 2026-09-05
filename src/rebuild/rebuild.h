#ifndef REBUILD_REBUILD_H
#define REBUILD_REBUILD_H

#include <cstddef>

#include "cluster/cluster.h"
#include "repository/repository.h"

namespace rebuild
{
	// How many records a page of a rebuild asks for. It is well under what a scan would give,
	// because these pages carry values: a thousand of them is a response built whole in memory at
	// both ends, and a value may be sixteen megabytes.
	constexpr size_t default_page = 100;

	// How long a rebuild is given before it gives up and lets the node start on what it has.
	//
	// The node is not in the membership while this runs, so a rebuild that never ends is a node
	// that never registers — a copy the cluster is waiting for and never gets, and one that
	// answers no health check while it waits. Every round trip inside is bounded already; this is
	// the bound on all of them together, for the arithmetic that says four timeouts is two minutes
	// being wrong about how many there are.
	//
	// A node that gives up part way is thin rather than empty, and it will not try again — an
	// empty store is the only trigger. That is the trade, and it is the right way round: thin and
	// serving is a copy the cluster has, where absent is one it does not.
	constexpr long default_seconds = 300;

	// Fills a store that holds nothing from a zone that still holds its records, and answers how
	// many records were written.
	//
	// A replaced instance is an empty database — the volume went with the instance — and nothing
	// in the cluster puts that copy back: there is no read repair, no anti-entropy, no hinted
	// handoff and no replication log. This is that step, done once, on the way up.
	//
	// **It runs before the node registers in etcd**, and that is what makes it free to take as
	// long as it needs. A node that is not in the membership is nobody's copy: no read is answered
	// from it, no write is waiting on it, and the keys it is about to own are still held and served
	// by the nodes that own them now. Registering first and rebuilding afterwards would be a node
	// that every write to its partitions has to wait for, and every copy has to take a write.
	//
	// A node that holds anything at all rebuilds nothing. Only a store with no tables in it is one
	// that was lost rather than kept, and reading a whole zone on every ordinary restart would cost
	// the keyspace to learn that nothing is missing.
	size_t rebuild(
		repository::repository &repository,
		const cluster::cluster &nodes,
		size_t page = default_page,
		long seconds = default_seconds);
}

#endif
