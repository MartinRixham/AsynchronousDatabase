#ifndef CLUSTER_PLACEMENTS_H
#define CLUSTER_PLACEMENTS_H

#include <array>
#include <optional>
#include <string>

#include "cluster.h"
#include "partition.h"

namespace cluster
{
	// Where the keys of each partition live, worked out once a partition instead of once a key.
	//
	// A pass that moves records asks where every key of a store belongs, and there are 256 answers
	// to that question rather than one for each key: replicas() is decided by the key's partition
	// and nothing else. Walking a million keys is otherwise a million copies of the membership and
	// a million hashes of it.
	//
	// One of these is a moment, and it is meant to be short lived: it is built for a single walk
	// and thrown away after it, so a membership that changes is seen by the next walk. Nothing
	// here decides whether giving a record up is safe — the node that owns it is asked that, and
	// asked afresh.
	class placements
	{
		const cluster &nodes;

		std::array<std::optional<placement>, partition_count> known;

	public:
		explicit placements(const cluster &nodes);

		const placement &of(const std::string &key);
	};
}

#endif
