#ifndef CLUSTER_PARTITION_H
#define CLUSTER_PARTITION_H

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <map>
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

	// The node that should lead a partition. It is the owner of the key across the whole
	// membership, and so the owner of it in one zone as well: a leader always holds a copy of what
	// it orders writes to. Every node works it out from the membership rather than racing for the
	// key, which is what makes leadership move when the membership does — a node that has just
	// joined is the answer for its share of the ring the moment it is in the list.
	std::string leader_of(const std::string &key, const std::vector<member> &members);

	std::vector<std::vector<std::string>> zones_of(
		const std::vector<member> &members,
		const std::string &node,
		const std::string &zone);

	// Which nodes of one zone hold these partitions, and which of them each node holds. A zone
	// holds a copy of the whole keyspace split between its nodes, so one node of it holds each
	// partition and the rest would answer a file with nothing of it in them.
	//
	// The list is the zone as the node asking sees it, so a zone this node is in is a zone with
	// this node left out — which makes the answer the node that held the partition before this
	// one did.
	std::map<std::string, partition_set> holders_in(
		const partition_set &partitions,
		const std::vector<std::string> &nodes);

	// Which nodes hold the records of these partitions, and which of the partitions to ask each of
	// them for. A zone holds a copy of the whole keyspace, so one node of every other zone holds
	// each of them; and in this node's own zone it is the owner among that zone's other nodes,
	// which is the node this one took the partition from.
	//
	// **It is bounded by the partitions and not by the membership**: the more nodes a zone has, the
	// smaller the share of it one node holds, so a share of 256 partitions never names more than
	// sixteen nodes of a zone however many are in it.
	//
	// Two nodes joining a zone at once can name each other rather than the node that held the
	// partition, which is a share fetched from the other zones instead — they are asked as well,
	// because a zone that lost a node has no copy of its share left to ask.
	std::map<std::string, partition_set> holders_of(
		const partition_set &partitions,
		const std::vector<member> &members,
		const std::string &node,
		const std::string &zone);
}

#endif
