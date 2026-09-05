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
	size_t rebuild(repository::repository &repository, const cluster::cluster &nodes, size_t page = default_page);
}

#endif
