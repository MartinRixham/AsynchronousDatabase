#include <map>

#include "partition.h"

namespace
{
	constexpr uint64_t fnv_offset = 14695981039346656037ULL;

	constexpr uint64_t fnv_prime = 1099511628211ULL;

	uint64_t hash(const std::string &text, uint64_t seed)
	{
		uint64_t hashed = seed;

		for (size_t i = 0; i < text.size(); i++)
		{
			hashed ^= static_cast<unsigned char>(text[i]);
			hashed *= fnv_prime;
		}

		return hashed;
	}

	// FNV carries most of what it knows in the bottom bits, and the highest score is what wins a
	// key here, so the hash is stirred before it is compared. Without this, keys named in a
	// series land on two of three nodes rather than on all of them.
	uint64_t mix(uint64_t hashed)
	{
		hashed ^= hashed >> 33;
		hashed *= 0xff51afd7ed558ccdULL;
		hashed ^= hashed >> 33;
		hashed *= 0xc4ceb9fe1a85ec53ULL;
		hashed ^= hashed >> 33;

		return hashed;
	}
}

// The node is hashed into the seed of the key's hash rather than concatenated with it, so that a
// node named for the start of a key cannot collide with a key that begins with the node's name.
uint64_t cluster::score(const std::string &node, const std::string &key)
{
	return mix(hash(key, hash(node, fnv_offset)));
}

std::string cluster::owner_of(const std::string &key, const std::vector<std::string> &nodes)
{
	std::string owner;
	uint64_t highest = 0;

	for (size_t i = 0; i < nodes.size(); i++)
	{
		uint64_t scored = score(nodes[i], key);

		// Two nodes scoring the same key alike is a coincidence rather than an error, and the
		// names break the tie so that every node agrees on which of them won.
		if (owner.empty() || scored > highest || (scored == highest && nodes[i] < owner))
		{
			owner = nodes[i];
			highest = scored;
		}
	}

	return owner;
}

std::vector<cluster::member> cluster::owners_of(const std::string &key, const std::vector<member> &members)
{
	// A map rather than a hash of them, because the zones come out in one order whatever order the
	// membership arrived in, and every node has to name the copies of a key alike.
	std::map<std::string, std::vector<std::string>> zones;

	for (size_t i = 0; i < members.size(); i++)
	{
		zones[members[i].zone].push_back(members[i].node);
	}

	std::vector<member> owners;

	for (std::map<std::string, std::vector<std::string>>::const_iterator it = zones.begin(); it != zones.end(); ++it)
	{
		owners.push_back(member { owner_of(key, it->second), it->first });
	}

	return owners;
}
