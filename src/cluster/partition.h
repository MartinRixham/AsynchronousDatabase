#ifndef CLUSTER_PARTITION_H
#define CLUSTER_PARTITION_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "member.h"

namespace cluster
{
	// A key belongs to a partition, and a partition is what a node holds, leads and moves. There
	// are enough of them that a zone's nodes get an even share and few enough that etcd holds a key
	// for each: the number is fixed for the life of a cluster, because changing it moves every key.
	constexpr size_t partition_count = 256;

	// The partition a key belongs to. The key alone decides, so the same key of two tables is in
	// one partition and is led, held and moved as one thing.
	size_t partition_of(const std::string &key);

	// The name a partition is hashed and registered under.
	std::string partition_name(size_t partition);

	uint64_t score(const std::string &node, const std::string &key);

	std::string owner_of(const std::string &key, const std::vector<std::string> &nodes);

	std::vector<member> owners_of(const std::string &key, const std::vector<member> &members);

	std::vector<std::vector<std::string>> zones_of(
		const std::vector<member> &members,
		const std::string &node,
		const std::string &zone);
}

#endif
