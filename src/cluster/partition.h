#ifndef CLUSTER_PARTITION_H
#define CLUSTER_PARTITION_H

#include <cstdint>
#include <string>
#include <vector>

namespace cluster
{
	// Rendezvous hashing: every node scores the key and the highest score owns it. A ring of
	// tokens would do as well, but this needs no ring to agree on — every node computes the same
	// answer from the membership alone — and adding or removing a node moves only the keys that
	// node wins or loses, rather than reshuffling the keyspace.
	uint64_t score(const std::string &node, const std::string &key);

	// The empty string when there are no nodes at all, which is an instance standing alone.
	std::string owner_of(const std::string &key, const std::vector<std::string> &nodes);
}

#endif
