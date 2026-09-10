#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <boost/json.hpp>

#include "base64/base64.h"
#include "cluster/partition.h"
#include "rebuild/rebuild.h"
#include "record/record.h"
#include "router/api_error.h"
#include "table/table.h"
#include "url/url.h"
#include "../cluster/fake_cluster.h"
#include "../repository/fake_repository.h"

namespace
{
	const std::string self = "http://one:8080";

	const std::string peer = "http://two:8080";

	const std::string other = "http://three:8080";

	// This node alone in its zone, and one node in a zone of its own holding the other copy.
	std::vector<cluster::member> two_zones()
	{
		return std::vector<cluster::member> {
			cluster::member { self, "one" },
			cluster::member { peer, "two" }
		};
	}

	router::response tables(const std::vector<std::string> &names)
	{
		boost::json::array listed;

		for (size_t i = 0; i < names.size(); i++)
		{
			listed.push_back(table::valid_table(names[i], std::vector<std::string>()).json);
		}

		return router::json_response(
			boost::beast::http::status::ok, boost::json::object { { "tables", listed } });
	}

	// A file as the node being read from would have answered with, which is that node's own store
	// exporting it. Both ends of a transfer are a repository, so the file a test hands back is one
	// a repository wrote rather than bytes made up here.
	router::response file(const std::vector<std::string> &keys, const std::string &next = "")
	{
		repository::fake_repository source;

		source.create_table(table::valid_table("account", std::vector<std::string>()));

		for (size_t i = 0; i < keys.size(); i++)
		{
			source.write_record("account", record::valid_record(keys[i], "value of " + keys[i]));
		}

		repository::share whole;

		whole.partitions.set();

		repository::extract taken = source.export_records("account", whole);

		return router::file_response(taken.file, taken.records, next.empty() ? "" : base64::encode(next));
	}

	// One worker, so that the order these tests read is the order they wrote. What several of them
	// do is transfer_test's to say.
	rebuild::outcome rebuilt(
		repository::repository &repository,
		const cluster::cluster &nodes,
		long seconds = rebuild::default_seconds)
	{
		return rebuild::rebuild(repository, nodes, seconds, 1);
	}

	// The partitions a request asked for, which is the whole of what decides what comes back.
	cluster::partition_set asked_for(const router::request &request)
	{
		return cluster::decode_partitions(url::read_parameter(request.query, "partitions"))
			.value_or(cluster::partition_set());
	}
}

TEST(rebuild_test, rebuilds_nothing_when_the_store_already_holds_a_table)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	repository.create_table(table::valid_table("account", std::vector<std::string>()));

	nodes.answer(peer, tables({ "account" }));

	// Whole, because a node that kept its store is not one that came up short of its share.
	EXPECT_TRUE(rebuilt(repository, nodes).whole);

	// Nothing was asked of anybody, because a node that kept its store has nothing to rebuild.
	EXPECT_TRUE(nodes.sent().empty());
}

TEST(rebuild_test, rebuilds_nothing_when_there_is_only_one_zone)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, std::vector<std::string> { self, peer });

	nodes.answer(peer, tables({ "account" }));

	EXPECT_TRUE(rebuilt(repository, nodes).whole);
	EXPECT_TRUE(nodes.sent().empty());
}

TEST(rebuild_test, rebuilds_nothing_when_the_instance_stands_alone)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, std::vector<std::string> { self });

	// A node with nowhere to read from owns every key it is asked about, so a miss it reports is
	// its store and not a share it never read.
	EXPECT_TRUE(rebuilt(repository, nodes).whole);
	EXPECT_TRUE(nodes.sent().empty());
}

TEST(rebuild_test, writes_the_tables_of_another_zone)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	nodes.answer_in_turn(peer, { tables({ "account", "summary" }), file({}) });

	rebuilt(repository, nodes);

	EXPECT_TRUE(repository.has_table("account"));
	EXPECT_TRUE(repository.has_table("summary"));
}

TEST(rebuild_test, writes_the_records_of_another_zone)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	nodes.answer_in_turn(peer, { tables({ "account" }), file({ "a", "b" }) });

	EXPECT_EQ(2u, rebuilt(repository, nodes).records);

	EXPECT_EQ("value of a", repository.read_record("account", "a").value_or(""));
	EXPECT_EQ("value of b", repository.read_record("account", "b").value_or(""));
}

// What a file carries is decided by the node asking for it and not by the node answering, so what
// this node will hold is what it names — and a node a moment behind in what it thinks the cluster
// is still sends the right records.
TEST(rebuild_test, asks_for_the_partitions_this_node_will_hold)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	// "b" is held by another node of this node's own zone, so it is not this node's to ask for: a
	// copy of it here would be a stale one the moment the key was written again.
	nodes.copies("b", std::vector<std::string> { other });

	nodes.answer_in_turn(peer, { tables({ "account" }), file({ "a" }) });

	EXPECT_EQ(1u, rebuilt(repository, nodes).records);

	const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();

	ASSERT_EQ(2u, sent.size());

	cluster::partition_set wanted = asked_for(sent[1].second);

	EXPECT_TRUE(wanted.test(cluster::partition_of("a")));
	EXPECT_FALSE(wanted.test(cluster::partition_of("b")));
}

TEST(rebuild_test, asks_for_a_file_of_the_table_rather_than_for_its_records)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	nodes.answer_in_turn(peer, { tables({ "account" }), file({ "a" }) });

	rebuilt(repository, nodes);

	const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();

	ASSERT_EQ(2u, sent.size());
	EXPECT_EQ((std::vector<std::string> { "table", "account", "file" }), sent[1].second.path);
}

TEST(rebuild_test, asks_for_the_next_file_from_the_key_the_one_before_it_reached)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	// The walk that wrote the first file stopped at "b", so the second begins again there.
	nodes.answer_in_turn(peer, {
		tables({ "account" }),
		file({ "a", "b" }, "b"),
		file({ "c" })
	});

	EXPECT_EQ(3u, rebuilt(repository, nodes).records);

	EXPECT_TRUE(repository.read_record("account", "a").has_value());
	EXPECT_TRUE(repository.read_record("account", "b").has_value());
	EXPECT_TRUE(repository.read_record("account", "c").has_value());

	const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();

	ASSERT_EQ(3u, sent.size());
	EXPECT_NE(std::string::npos, sent[2].second.query.find("from=" + url::encode(base64::encode("b"))));
}

TEST(rebuild_test, asks_every_node_of_the_zone_it_reads_from)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, std::vector<cluster::member> {
		cluster::member { self, "one" },
		cluster::member { peer, "two" },
		cluster::member { other, "two" }
	});

	nodes.answer_in_turn(peer, { tables({ "account" }), file({ "a" }) });
	nodes.answer_in_turn(other, { file({ "b" }) });

	// A zone holds a copy of the whole keyspace between its nodes, so both of them are asked.
	EXPECT_EQ(2u, rebuilt(repository, nodes).records);

	EXPECT_TRUE(repository.read_record("account", "a").has_value());
	EXPECT_TRUE(repository.read_record("account", "b").has_value());
}

TEST(rebuild_test, gives_up_on_a_zone_whose_node_does_not_answer)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	nodes.answer_in_turn(peer, {
		tables({ "account" }),
		router::error_response("storage_error", "Node did not answer.")
	});

	// Short of its share: the count alone says nothing, because a node that needed nothing takes
	// no records either.
	EXPECT_FALSE(rebuilt(repository, nodes).whole);

	// The table is still declared, because a node that holds no table can hold no record either.
	EXPECT_TRUE(repository.has_table("account"));
	EXPECT_FALSE(repository.read_record("account", "a").has_value());
}

TEST(rebuild_test, asks_the_next_zone_when_one_of_them_does_not_answer)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, std::vector<cluster::member> {
		cluster::member { self, "one" },
		cluster::member { peer, "two" },
		cluster::member { other, "three" }
	});

	nodes.answer_in_turn(peer, { router::error_response("storage_error", "Node did not answer.") });
	nodes.answer_in_turn(other, { tables({ "account" }), file({ "a" }) });

	rebuild::outcome taken = rebuilt(repository, nodes);

	EXPECT_EQ(1u, taken.records);
	EXPECT_TRUE(taken.whole);
	EXPECT_TRUE(repository.read_record("account", "a").has_value());
}

TEST(rebuild_test, stops_rather_than_asking_for_ever_when_a_file_does_not_advance)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	// A file that ends where the one before it did, and says there is more, is a walk that asking
	// again would ask the same thing of for ever.
	nodes.answer_in_turn(peer, {
		tables({ "account" }),
		file({ "a" }, "a"),
		file({ "a" }, "a")
	});

	// A rebuild that did not read a whole zone answers nothing, and the node starts thin: what it
	// took is still its own, and there is no zone left to ask.
	rebuild::outcome taken = rebuilt(repository, nodes);

	EXPECT_EQ(0u, taken.records);
	EXPECT_FALSE(taken.whole);
	EXPECT_EQ(3u, nodes.sent().size());
	EXPECT_TRUE(repository.read_record("account", "a").has_value());
}

TEST(rebuild_test, resumes_from_a_key_that_has_to_be_encoded)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	nodes.answer_in_turn(peer, {
		tables({ "account" }),
		file({ "a", "a b/c" }, "a b/c"),
		file({ "d" })
	});

	EXPECT_EQ(3u, rebuilt(repository, nodes).records);

	const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();

	ASSERT_EQ(3u, sent.size());
	EXPECT_NE(
		std::string::npos,
		sent[2].second.query.find("from=" + url::encode(base64::encode("a b/c"))));
}

// A node is out of the membership until its rebuild is done, so a rebuild being answered nothing
// is a copy the cluster waits for and never gets. Each round trip is bounded already; this is the
// bound on a node that has stopped answering between them.
TEST(rebuild_test, gives_up_when_it_is_answered_nothing)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	nodes.answer_in_turn(peer, { tables({ "account" }), file({ "a", "b" }) });

	// And a rebuild that gave up is told from one that had nothing to do, which is what the node
	// answers a read with while it is short.
	EXPECT_FALSE(rebuilt(repository, nodes, 0).whole);

	// Nothing was asked of the zone, rather than asked and thrown away.
	EXPECT_TRUE(nodes.sent().empty());
	EXPECT_FALSE(repository.has_table("account"));
}

// Patience is not a deadline: a rebuild still being sent files goes on being sent them, because
// how long one takes is how much there is to read and no wall clock is right for every store.
TEST(rebuild_test, keeps_going_while_the_files_keep_arriving)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	nodes.answer_in_turn(peer, { tables({ "account" }), file({ "a" }, "a"), file({ "b" }) });

	// Three answers, each of them longer than the whole of the patience.
	nodes.slow(peer, std::chrono::milliseconds(700));

	EXPECT_EQ(2u, rebuilt(repository, nodes, 1).records);

	EXPECT_TRUE(repository.read_record("account", "a").has_value());
	EXPECT_TRUE(repository.read_record("account", "b").has_value());
}
