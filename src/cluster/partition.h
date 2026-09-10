#ifndef CLUSTER_PARTITION_H
#define CLUSTER_PARTITION_H

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "member.h"

namespace cluster
{
	// A key belongs to a partition, and a partition is what a node holds, leads and moves. There
	// are enough of them that a zone's nodes get an even share and few enough that etcd holds a key
	// for each: the number is fixed for the life of a cluster, because changing it moves every key.
	constexpr size_t partition_count = 256;

	// The partition a key belongs to. The partition key alone decides, so the same key of two
	// tables is in one partition and is led, held and moved as one thing — and so is every record
	// of one partition key, whatever it sorts under.
	size_t partition_of(const std::string &key);

	// The partitions a node holds, as a set. A node asking another for its share of a table names
	// what it holds rather than a key at a time: there is no key to ask about until the answer has
	// arrived, and where a key belongs is decided by its partition anyway.
	typedef std::bitset<partition_count> partition_set;

	// A partition set as it travels, four partitions to a hexadecimal character and the highest
	// numbered first. It is fixed width, so a query carrying one is the same length whatever the
	// node asking happens to hold.
	std::string encode_partitions(const partition_set &partitions);

	// Nothing when the text is not a set of exactly this many partitions, which is a request from
	// a node that does not agree with this one about how the keyspace is cut up.
	std::optional<partition_set> decode_partitions(const std::string &text);

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
