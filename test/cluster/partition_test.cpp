#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cluster/partition.h"

namespace
{
	const std::vector<std::string> three {
		"http://asyncdb-1:8080",
		"http://asyncdb-2:8080",
		"http://asyncdb-3:8080"
	};

	std::string key(size_t number)
	{
		return "account/" + std::to_string(number);
	}

	// Six nodes, two in each of three zones, which is the shape of a deployment across three
	// availability zones.
	const std::vector<cluster::member> zoned {
		cluster::member { "http://asyncdb-1:8080", "a" },
		cluster::member { "http://asyncdb-2:8080", "a" },
		cluster::member { "http://asyncdb-3:8080", "b" },
		cluster::member { "http://asyncdb-4:8080", "b" },
		cluster::member { "http://asyncdb-5:8080", "c" },
		cluster::member { "http://asyncdb-6:8080", "c" }
	};

	std::vector<cluster::member> zoneless(const std::vector<std::string> &nodes)
	{
		std::vector<cluster::member> members;

		for (size_t i = 0; i < nodes.size(); i++)
		{
			members.push_back(cluster::member { nodes[i], "" });
		}

		return members;
	}

	std::vector<cluster::member> without(const std::vector<cluster::member> &members, const std::string &node)
	{
		std::vector<cluster::member> left;

		for (size_t i = 0; i < members.size(); i++)
		{
			if (members[i].node != node)
			{
				left.push_back(members[i]);
			}
		}

		return left;
	}

	std::vector<std::string> nodes_of(const std::vector<cluster::member> &members)
	{
		std::vector<std::string> nodes;

		for (size_t i = 0; i < members.size(); i++)
		{
			nodes.push_back(members[i].node);
		}

		return nodes;
	}

	std::string owner_in(const std::vector<cluster::member> &owners, const std::string &zone)
	{
		std::vector<cluster::member>::const_iterator found = std::find_if(
			owners.begin(),
			owners.end(),
			[&zone](const cluster::member &owner) { return owner.zone == zone; });

		return found == owners.end() ? "" : found->node;
	}
}

TEST(partition_test, no_node_owns_a_key_when_there_are_no_nodes)
{
	EXPECT_EQ(cluster::owner_of("key", std::vector<std::string>()), "");
}

TEST(partition_test, one_node_owns_every_key)
{
	std::vector<std::string> one { "http://asyncdb-1:8080" };

	for (size_t i = 0; i < 100; i++)
	{
		EXPECT_EQ(cluster::owner_of(key(i), one), "http://asyncdb-1:8080");
	}
}

TEST(partition_test, the_same_key_belongs_to_the_same_node_every_time)
{
	for (size_t i = 0; i < 100; i++)
	{
		EXPECT_EQ(cluster::owner_of(key(i), three), cluster::owner_of(key(i), three));
	}
}

TEST(partition_test, the_order_of_the_nodes_does_not_decide)
{
	std::vector<std::string> reversed(three.rbegin(), three.rend());

	for (size_t i = 0; i < 100; i++)
	{
		EXPECT_EQ(cluster::owner_of(key(i), three), cluster::owner_of(key(i), reversed));
	}
}

TEST(partition_test, every_node_owns_a_share_of_the_keys)
{
	std::set<std::string> owners;

	for (size_t i = 0; i < 100; i++)
	{
		owners.insert(cluster::owner_of(key(i), three));
	}

	EXPECT_EQ(owners.size(), three.size());
}

TEST(partition_test, the_shares_are_of_a_size)
{
	std::map<std::string, size_t> counts;

	for (size_t i = 0; i < 3000; i++)
	{
		counts[cluster::owner_of(key(i), three)]++;
	}

	// A thousand each, and a fifth either way is a hash that spreads keys rather than one that
	// happens to agree with the way they are named.
	for (std::map<std::string, size_t>::const_iterator it = counts.begin(); it != counts.end(); ++it)
	{
		EXPECT_GT(it->second, 800u);
		EXPECT_LT(it->second, 1200u);
	}
}

// The reason for hashing this way rather than dividing the keyspace up: what a node held is what
// moves when it goes, and every other key stays where it was.
TEST(partition_test, losing_a_node_moves_only_the_keys_that_node_held)
{
	std::vector<std::string> two { three[0], three[1] };

	for (size_t i = 0; i < 1000; i++)
	{
		std::string before = cluster::owner_of(key(i), three);
		std::string after = cluster::owner_of(key(i), two);

		if (before != three[2])
		{
			EXPECT_EQ(before, after);
		}
		else
		{
			EXPECT_NE(after, three[2]);
		}
	}
}

TEST(partition_test, gaining_a_node_moves_only_the_keys_that_node_takes)
{
	std::vector<std::string> four(three);

	four.push_back("http://asyncdb-4:8080");

	for (size_t i = 0; i < 1000; i++)
	{
		std::string before = cluster::owner_of(key(i), three);
		std::string after = cluster::owner_of(key(i), four);

		if (after != "http://asyncdb-4:8080")
		{
			EXPECT_EQ(before, after);
		}
	}
}

TEST(partition_test, a_node_scores_a_key_differently_from_its_neighbours)
{
	EXPECT_NE(cluster::score(three[0], "key"), cluster::score(three[1], "key"));
}

// The whole of it: a record is in every zone, once.
TEST(partition_test, every_zone_holds_one_copy_of_a_key)
{
	for (size_t i = 0; i < 100; i++)
	{
		std::vector<cluster::member> owners = cluster::owners_of(key(i), zoned);
		std::set<std::string> zones;

		ASSERT_EQ(owners.size(), 3u);

		for (size_t j = 0; j < owners.size(); j++)
		{
			zones.insert(owners[j].zone);
		}

		EXPECT_EQ(zones, (std::set<std::string> { "a", "b", "c" }));
	}
}

TEST(partition_test, the_copy_in_a_zone_is_held_by_a_node_of_that_zone)
{
	for (size_t i = 0; i < 100; i++)
	{
		std::vector<cluster::member> owners = cluster::owners_of(key(i), zoned);

		for (size_t j = 0; j < owners.size(); j++)
		{
			bool found = false;

			for (size_t k = 0; k < zoned.size(); k++)
			{
				found = found || (zoned[k].node == owners[j].node && zoned[k].zone == owners[j].zone);
			}

			EXPECT_TRUE(found);
		}
	}
}

// A cluster that was never told about zones is every node in one zone, and one copy of a key.
TEST(partition_test, members_in_no_zone_hold_one_copy_between_them)
{
	std::vector<cluster::member> members = zoneless(three);

	for (size_t i = 0; i < 100; i++)
	{
		std::vector<cluster::member> owners = cluster::owners_of(key(i), members);

		ASSERT_EQ(owners.size(), 1u);
		EXPECT_EQ(owners[0].node, cluster::owner_of(key(i), three));
		EXPECT_EQ(owners[0].zone, "");
	}
}

TEST(partition_test, no_node_holds_a_key_when_there_are_no_nodes)
{
	EXPECT_TRUE(cluster::owners_of("key", std::vector<cluster::member>()).empty());
}

TEST(partition_test, the_order_of_the_members_does_not_decide)
{
	std::vector<cluster::member> reversed(zoned.rbegin(), zoned.rend());

	for (size_t i = 0; i < 100; i++)
	{
		EXPECT_EQ(nodes_of(cluster::owners_of(key(i), zoned)), nodes_of(cluster::owners_of(key(i), reversed)));
	}
}

// Every zone decides its own copy, so a zone losing a node is a zone's worth of keys moving and
// the other zones holding what they held. That is what makes losing a zone survivable rather than
// a reshuffle of the whole cluster.
TEST(partition_test, a_node_leaving_one_zone_moves_no_copy_in_another)
{
	std::vector<cluster::member> left = without(zoned, "http://asyncdb-3:8080");

	for (size_t i = 0; i < 1000; i++)
	{
		std::vector<cluster::member> before = cluster::owners_of(key(i), zoned);
		std::vector<cluster::member> after = cluster::owners_of(key(i), left);

		EXPECT_EQ(owner_in(before, "a"), owner_in(after, "a"));
		EXPECT_EQ(owner_in(before, "c"), owner_in(after, "c"));
		EXPECT_EQ(owner_in(after, "b"), "http://asyncdb-4:8080");
	}
}

// A zone that is gone is a copy that is gone, and the copies in the zones that are left stay where
// they are — which is what a node reads from when a zone is down.
TEST(partition_test, a_zone_leaving_moves_no_copy_in_another)
{
	std::vector<cluster::member> left = without(without(zoned, "http://asyncdb-5:8080"), "http://asyncdb-6:8080");

	for (size_t i = 0; i < 1000; i++)
	{
		std::vector<cluster::member> before = cluster::owners_of(key(i), zoned);
		std::vector<cluster::member> after = cluster::owners_of(key(i), left);

		ASSERT_EQ(after.size(), 2u);
		EXPECT_EQ(owner_in(before, "a"), owner_in(after, "a"));
		EXPECT_EQ(owner_in(before, "b"), owner_in(after, "b"));
	}
}

TEST(partition_test, every_node_of_a_zone_holds_a_share_of_its_copies)
{
	std::set<std::string> owners;

	for (size_t i = 0; i < 100; i++)
	{
		owners.insert(owner_in(cluster::owners_of(key(i), zoned), "a"));
	}

	EXPECT_EQ(owners, (std::set<std::string> { "http://asyncdb-1:8080", "http://asyncdb-2:8080" }));
}
