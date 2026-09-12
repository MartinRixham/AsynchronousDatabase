#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cluster/partition.h"
#include "record/record.h"

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

	std::string zone_of(const std::vector<cluster::member> &members, const std::string &node)
	{
		std::vector<cluster::member>::const_iterator found = std::find_if(
			members.begin(),
			members.end(),
			[&node](const cluster::member &member) { return member.node == node; });

		return found == members.end() ? "" : found->zone;
	}

	// The partitions a node holds, which is every one it owns in its own zone.
	cluster::partition_set held_by(const std::vector<cluster::member> &members, const std::string &node)
	{
		cluster::partition_set held;

		for (size_t partition = 0; partition < cluster::partition_count; partition++)
		{
			if (owner_in(cluster::owners_of(cluster::partition_name(partition), members), zone_of(members, node)) ==
				node)
			{
				held.set(partition);
			}
		}

		return held;
	}

	// A deployment of several zones with several nodes in each.
	std::vector<cluster::member> spread(size_t zones, size_t per_zone)
	{
		std::vector<cluster::member> members;

		for (size_t zone = 0; zone < zones; zone++)
		{
			for (size_t i = 0; i < per_zone; i++)
			{
				members.push_back(cluster::member {
					"http://asyncdb-" + std::to_string(zone * per_zone + i) + ":8080",
					std::string(1, static_cast<char>('a' + zone)) });
			}
		}

		return members;
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

// A write is ordered by the node leading the key's partition and then written to every copy, so a
// leader that held no copy would be a hop the write did not need.
TEST(partition_test, the_leader_of_a_key_is_one_of_the_nodes_that_hold_it)
{
	for (size_t i = 0; i < 1000; i++)
	{
		std::string leader = cluster::leader_of(key(i), zoned);
		std::vector<cluster::member> owners = cluster::owners_of(key(i), zoned);

		EXPECT_TRUE(
			std::any_of(
				owners.begin(),
				owners.end(),
				[&leader](const cluster::member &owner) { return owner.node == leader; }));
	}
}

// Leadership is spread the way the keyspace is, so no node orders the writes of a larger part of it
// than the others — and a node that joins is named to lead a share of it at once, rather than
// leading whatever it happened to claim first.
TEST(partition_test, every_node_leads_a_share_of_the_partitions)
{
	std::map<std::string, size_t> led;

	for (size_t i = 0; i < cluster::partition_count; i++)
	{
		led[cluster::leader_of(cluster::partition_name(i), zoned)]++;
	}

	ASSERT_EQ(led.size(), zoned.size());

	for (std::map<std::string, size_t>::const_iterator it = led.begin(); it != led.end(); ++it)
	{
		EXPECT_GT(it->second, cluster::partition_count / (2 * zoned.size()));
	}
}

// A node added to a cluster is named to lead partitions the moment it is in the membership, and the
// nodes that led them are the ones that give them up.
TEST(partition_test, a_node_that_joins_is_named_to_lead_partitions_it_did_not_before)
{
	std::vector<cluster::member> joined = zoned;
	std::string arrival = "http://asyncdb-7:8080";
	size_t leads = 0;

	joined.push_back(cluster::member { arrival, "a" });

	for (size_t i = 0; i < cluster::partition_count; i++)
	{
		if (cluster::leader_of(cluster::partition_name(i), joined) == arrival)
		{
			leads++;
		}
	}

	EXPECT_GT(leads, 0u);
}

// A scan asks one zone, because one zone holds every key, and it asks its own first: those nodes
// are in the same availability zone, and a page from them crosses no boundary.
TEST(partition_test, the_zones_of_a_scan_begin_with_this_node_s_own)
{
	std::vector<std::vector<std::string>> zones =
		cluster::zones_of(zoned, "http://asyncdb-1:8080", "a");

	ASSERT_EQ(zones.size(), 3u);
	EXPECT_EQ(zones[0], std::vector<std::string>({ "http://asyncdb-2:8080" }));
	EXPECT_EQ(zones[1], (std::vector<std::string> { "http://asyncdb-3:8080", "http://asyncdb-4:8080" }));
	EXPECT_EQ(zones[2], (std::vector<std::string> { "http://asyncdb-5:8080", "http://asyncdb-6:8080" }));
}

// A node that is the only one in its zone holds every key itself, so the zone it asks first is no
// node at all.
TEST(partition_test, a_node_alone_in_its_zone_asks_nobody_for_a_scan)
{
	std::vector<cluster::member> one_each {
		cluster::member { "http://asyncdb-1:8080", "a" },
		cluster::member { "http://asyncdb-2:8080", "b" },
		cluster::member { "http://asyncdb-3:8080", "c" }
	};

	std::vector<std::vector<std::string>> zones =
		cluster::zones_of(one_each, "http://asyncdb-1:8080", "a");

	ASSERT_EQ(zones.size(), 3u);
	EXPECT_TRUE(zones[0].empty());
	EXPECT_EQ(zones[1], std::vector<std::string>({ "http://asyncdb-2:8080" }));
	EXPECT_EQ(zones[2], std::vector<std::string>({ "http://asyncdb-3:8080" }));
}

// A cluster that was never told about zones is one zone of every node, so a scan asks all of them.
TEST(partition_test, members_in_no_zone_are_one_zone_of_every_other_node)
{
	std::vector<std::vector<std::string>> zones = cluster::zones_of(zoneless(three), three[0], "");

	ASSERT_EQ(zones.size(), 1u);
	EXPECT_EQ(zones[0], (std::vector<std::string> { three[1], three[2] }));
}

TEST(partition_test, the_zones_of_a_scan_hold_every_node_but_this_one)
{
	std::vector<std::vector<std::string>> zones =
		cluster::zones_of(zoned, "http://asyncdb-3:8080", "b");
	std::set<std::string> asked;

	for (size_t i = 0; i < zones.size(); i++)
	{
		for (size_t j = 0; j < zones[i].size(); j++)
		{
			asked.insert(zones[i][j]);
		}
	}

	EXPECT_EQ(asked.size(), zoned.size() - 1);
	EXPECT_EQ(asked.count("http://asyncdb-3:8080"), 0u);
}

TEST(partition_test, a_partition_set_survives_being_written_down_and_read_back)
{
	cluster::partition_set held;

	held.set(0);
	held.set(1);
	held.set(97);
	held.set(cluster::partition_count - 1);

	std::optional<cluster::partition_set> read = cluster::decode_partitions(cluster::encode_partitions(held));

	ASSERT_TRUE(read.has_value());
	EXPECT_EQ(held, *read);
}

TEST(partition_test, a_partition_set_is_written_down_at_one_length_whatever_is_in_it)
{
	cluster::partition_set none;
	cluster::partition_set every;

	every.set();

	EXPECT_EQ(cluster::encode_partitions(none).size(), cluster::encode_partitions(every).size());
	EXPECT_EQ(cluster::partition_count / 4, cluster::encode_partitions(every).size());
}

TEST(partition_test, the_empty_set_and_the_whole_of_it_are_read_back_as_themselves)
{
	cluster::partition_set every;

	every.set();

	EXPECT_EQ(0u, cluster::decode_partitions(cluster::encode_partitions(cluster::partition_set()))->count());
	EXPECT_EQ(cluster::partition_count, cluster::decode_partitions(cluster::encode_partitions(every))->count());
}

// A set this cluster does not agree with is a node asking for a keyspace cut up some other way,
// which is refused rather than read as whatever the first characters happen to say.
TEST(partition_test, a_set_that_is_not_this_many_partitions_is_no_set_at_all)
{
	std::string whole = cluster::encode_partitions(cluster::partition_set());

	EXPECT_FALSE(cluster::decode_partitions("").has_value());
	EXPECT_FALSE(cluster::decode_partitions(whole.substr(1)).has_value());
	EXPECT_FALSE(cluster::decode_partitions(whole + "0").has_value());
	EXPECT_FALSE(cluster::decode_partitions(whole.substr(1) + "q").has_value());
}

// The whole of what the split is for: a partition key's records are one partition, so they are
// held, led and moved together however many of them there are and whatever they sort under.
TEST(partition_test, every_sort_key_of_one_partition_key_is_in_one_partition)
{
	size_t partition = cluster::partition_of("4821");

	for (size_t i = 0; i < 1000; i++)
	{
		EXPECT_EQ(cluster::partition_of(record::compose_key("4821", std::to_string(i))), partition);
	}
}

// The other half of it: what the partition key does not decide is nothing, so keys that differ
// only there are spread the way any keys are.
TEST(partition_test, partition_keys_are_spread_over_the_partitions)
{
	std::set<size_t> partitions;

	for (size_t i = 0; i < 5000; i++)
	{
		partitions.insert(cluster::partition_of(record::compose_key(std::to_string(i), "2019")));
	}

	EXPECT_EQ(partitions.size(), cluster::partition_count);
}

// A zone's copy is split between its nodes, so a partition is asked of the one that holds it. What
// the rest would answer is a file with nothing of the share in it, having read their whole table to
// find that out.
TEST(partition_test, a_partition_is_asked_of_the_one_node_of_a_zone_that_holds_it)
{
	const std::vector<std::string> zone { "http://asyncdb-3:8080", "http://asyncdb-4:8080" };
	cluster::partition_set every;

	every.set();

	std::map<std::string, cluster::partition_set> holders = cluster::holders_in(every, zone);

	ASSERT_EQ(holders.size(), zone.size());

	cluster::partition_set asked;

	for (std::map<std::string, cluster::partition_set>::const_iterator it = holders.begin();
		it != holders.end();
		++it)
	{
		// No partition is asked of two nodes of one zone: the two sets have nothing in common, and
		// between them they are the whole of what was asked about.
		EXPECT_TRUE((asked & it->second).none());

		asked |= it->second;

		for (size_t partition = 0; partition < cluster::partition_count; partition++)
		{
			if (it->second.test(partition))
			{
				EXPECT_EQ(cluster::owner_of(cluster::partition_name(partition), zone), it->first);
			}
		}
	}

	EXPECT_TRUE(asked.all());
}

// A zone of no nodes at all, which is the zone a node alone in its own is asked to read from.
TEST(partition_test, a_zone_of_no_nodes_holds_nothing_to_ask_for)
{
	cluster::partition_set every;

	every.set();

	EXPECT_TRUE(cluster::holders_in(every, std::vector<std::string>()).empty());
}

// Every zone holds a copy of a partition, and one node of the zone holds it, so a share is asked
// of one node of each of them and not of every node of any.
TEST(partition_test, a_partition_is_asked_of_one_node_of_every_zone)
{
	const std::string node = "http://asyncdb-1:8080";
	cluster::partition_set held = held_by(zoned, node);
	std::map<std::string, cluster::partition_set> holders = cluster::holders_of(held, zoned, node, "a");

	ASSERT_GT(held.count(), 0u);

	for (size_t partition = 0; partition < cluster::partition_count; partition++)
	{
		if (!held.test(partition))
		{
			continue;
		}

		std::multiset<std::string> asked;

		for (std::map<std::string, cluster::partition_set>::const_iterator it = holders.begin();
			it != holders.end();
			++it)
		{
			if (it->second.test(partition))
			{
				asked.insert(zone_of(zoned, it->first));
			}
		}

		EXPECT_EQ(asked, (std::multiset<std::string> { "a", "b", "c" }));
	}
}

// The node asked in this node's own zone is the one the partition was taken from: a zone that
// gains a node redraws its split, and the records are on whichever of its other nodes owned the
// partition while this one was not there.
TEST(partition_test, the_node_asked_in_this_node_s_own_zone_held_the_partition_before_it)
{
	const std::string node = "http://asyncdb-1:8080";
	cluster::partition_set held = held_by(zoned, node);
	std::map<std::string, cluster::partition_set> holders = cluster::holders_of(held, zoned, node, "a");
	std::vector<cluster::member> before = without(zoned, node);
	size_t asked = 0;

	for (size_t partition = 0; partition < cluster::partition_count; partition++)
	{
		if (!held.test(partition))
		{
			continue;
		}

		std::string took_from =
			owner_in(cluster::owners_of(cluster::partition_name(partition), before), "a");

		ASSERT_FALSE(took_from.empty());
		EXPECT_TRUE(holders[took_from].test(partition));

		asked++;
	}

	EXPECT_GT(asked, 0u);
}

// What grouping a share by the node that holds it is for. A zone of many nodes splits the keyspace
// so finely that one node's share of it names few of them, and every node a pass asks reads its
// whole table to answer — so asking all of them is a cluster reading itself through once a node
// for every membership change.
TEST(partition_test, a_share_of_a_large_cluster_is_asked_of_a_handful_of_its_nodes)
{
	std::vector<cluster::member> large = spread(3, 32);
	const std::string node = large[0].node;
	cluster::partition_set held = held_by(large, node);
	std::map<std::string, cluster::partition_set> holders = cluster::holders_of(held, large, node, "a");

	// One node of each zone for each partition held, and a share of a cluster this size is a
	// handful of partitions.
	EXPECT_LE(holders.size(), 3 * held.count());
	EXPECT_LT(holders.size(), large.size() / 2);
}

// A node that is the only one of its zone took its share from nobody there, so what it asks is the
// other zones — which is the only place a zone that lost a node can be filled from.
TEST(partition_test, a_node_alone_in_its_zone_asks_only_the_other_zones)
{
	std::vector<cluster::member> one_each {
		cluster::member { "http://asyncdb-1:8080", "a" },
		cluster::member { "http://asyncdb-2:8080", "b" },
		cluster::member { "http://asyncdb-3:8080", "c" }
	};
	cluster::partition_set every;

	every.set();

	std::map<std::string, cluster::partition_set> holders =
		cluster::holders_of(every, one_each, "http://asyncdb-1:8080", "a");

	ASSERT_EQ(holders.size(), 2u);
	EXPECT_TRUE(holders["http://asyncdb-2:8080"].all());
	EXPECT_TRUE(holders["http://asyncdb-3:8080"].all());
}

TEST(partition_test, no_node_is_asked_about_a_partition_the_share_does_not_name)
{
	cluster::partition_set one;

	one.set(7);

	std::map<std::string, cluster::partition_set> holders =
		cluster::holders_of(one, zoned, "http://asyncdb-1:8080", "a");

	ASSERT_FALSE(holders.empty());

	for (std::map<std::string, cluster::partition_set>::const_iterator it = holders.begin();
		it != holders.end();
		++it)
	{
		EXPECT_EQ(it->second.count(), 1u);
		EXPECT_TRUE(it->second.test(7));
	}
}

// An instance standing alone holds every key and has nobody to ask for any of it.
TEST(partition_test, a_node_that_is_the_whole_membership_has_nobody_to_ask)
{
	std::vector<cluster::member> alone { cluster::member { "http://asyncdb-1:8080", "a" } };
	cluster::partition_set every;

	every.set();

	EXPECT_TRUE(cluster::holders_of(every, alone, "http://asyncdb-1:8080", "a").empty());
}
