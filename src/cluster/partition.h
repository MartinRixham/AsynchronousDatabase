#ifndef CLUSTER_PARTITION_H
#define CLUSTER_PARTITION_H

#include <cstdint>
#include <string>
#include <vector>

#include "member.h"

namespace cluster
{
	// Rendezvous hashing: every node scores the key and the highest score owns it. A ring of
	// tokens would do as well, but this needs no ring to agree on — every node computes the same
	// answer from the membership alone — and adding or removing a node moves only the keys that
	// node wins or loses, rather than reshuffling the keyspace.
	uint64_t score(const std::string &node, const std::string &key);

	// The empty string when there are no nodes at all, which is an instance standing alone.
	std::string owner_of(const std::string &key, const std::vector<std::string> &nodes);

	// Where the copies of a key live: the owner of the key in each zone, one copy per zone and no
	// zone holding two. The zones are decided one at a time, so a zone that loses or gains a node
	// moves only its own copies, and the other zones keep theirs where they are.
	//
	// Members that name no zone are one zone between them, so a cluster that was never told about
	// zones answers with the one owner it always did.
	std::vector<member> owners_of(const std::string &key, const std::vector<member> &members);
}

#endif
