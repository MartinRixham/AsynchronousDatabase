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
