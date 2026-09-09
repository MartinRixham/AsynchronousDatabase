#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <boost/json.hpp>

#include "rebuild/rebuild.h"
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

	router::response page(const std::vector<std::string> &keys, bool has_more)
	{
		boost::json::array records;

		for (size_t i = 0; i < keys.size(); i++)
		{
			records.push_back(boost::json::object {
				{ "key", boost::json::string(keys[i]) },
				{ "value", boost::json::string("value of " + keys[i]) }
			});
		}

		boost::json::object body { { "records", records } };

		if (has_more)
		{
			body["next"] = boost::json::string("a cursor");
		}

		return router::json_response(boost::beast::http::status::ok, body);
	}
}

TEST(rebuild_test, rebuilds_nothing_when_the_store_already_holds_a_table)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	repository.create_table(table::valid_table("account", std::vector<std::string>()));

	nodes.answer(peer, tables({ "account" }));

	EXPECT_EQ(0u, rebuild::rebuild(repository, nodes));

	// Nothing was asked of anybody, because a node that kept its store has nothing to rebuild.
	EXPECT_TRUE(nodes.sent().empty());
}

TEST(rebuild_test, rebuilds_nothing_when_there_is_only_one_zone)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, std::vector<std::string> { self, peer });

	nodes.answer(peer, tables({ "account" }));

	EXPECT_EQ(0u, rebuild::rebuild(repository, nodes));
	EXPECT_TRUE(nodes.sent().empty());
}

TEST(rebuild_test, rebuilds_nothing_when_the_instance_stands_alone)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, std::vector<std::string> { self });

	EXPECT_EQ(0u, rebuild::rebuild(repository, nodes));
	EXPECT_TRUE(nodes.sent().empty());
}

TEST(rebuild_test, writes_the_tables_of_another_zone)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	nodes.answer_in_turn(peer, { tables({ "account", "summary" }), page({}, false) });

	rebuild::rebuild(repository, nodes);

	EXPECT_TRUE(repository.has_table("account"));
	EXPECT_TRUE(repository.has_table("summary"));
}

TEST(rebuild_test, writes_the_records_of_another_zone)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	nodes.answer_in_turn(peer, { tables({ "account" }), page({ "a", "b" }, false) });

	EXPECT_EQ(2u, rebuild::rebuild(repository, nodes));

	EXPECT_EQ("value of a", repository.read_record("account", "a").value_or(""));
	EXPECT_EQ("value of b", repository.read_record("account", "b").value_or(""));
}

TEST(rebuild_test, writes_only_the_records_this_node_will_hold)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	// "b" is held by another node of this node's own zone, so it is not this node's to write: a
	// copy of it here would be a stale one the moment the key was written again.
	nodes.copies("b", std::vector<std::string> { other });

	nodes.answer_in_turn(peer, { tables({ "account" }), page({ "a", "b" }, false) });

	EXPECT_EQ(1u, rebuild::rebuild(repository, nodes));

	EXPECT_TRUE(repository.read_record("account", "a").has_value());
	EXPECT_FALSE(repository.read_record("account", "b").has_value());
}

TEST(rebuild_test, pages_through_a_scan_from_the_last_key_of_the_page_before)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	// A bound is inclusive, so the second page begins again with the key the first one ended at.
	nodes.answer_in_turn(peer, {
		tables({ "account" }),
		page({ "a", "b" }, true),
		page({ "b", "c" }, false)
	});

	EXPECT_EQ(3u, rebuild::rebuild(repository, nodes));

	EXPECT_TRUE(repository.read_record("account", "a").has_value());
	EXPECT_TRUE(repository.read_record("account", "b").has_value());
	EXPECT_TRUE(repository.read_record("account", "c").has_value());

	// The second page asked to resume at the last key of the first.
	const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();

	ASSERT_EQ(3u, sent.size());
	EXPECT_NE(std::string::npos, sent[2].second.query.find("from=b"));
}

TEST(rebuild_test, asks_every_node_of_the_zone_it_reads_from)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, std::vector<cluster::member> {
		cluster::member { self, "one" },
		cluster::member { peer, "two" },
		cluster::member { other, "two" }
	});

	nodes.answer_in_turn(peer, { tables({ "account" }), page({ "a" }, false) });
	nodes.answer_in_turn(other, { page({ "b" }, false) });

	// A zone holds a copy of the whole keyspace between its nodes, so both of them are asked.
	EXPECT_EQ(2u, rebuild::rebuild(repository, nodes));

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

	EXPECT_EQ(0u, rebuild::rebuild(repository, nodes));

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
	nodes.answer_in_turn(other, { tables({ "account" }), page({ "a" }, false) });

	EXPECT_EQ(1u, rebuild::rebuild(repository, nodes));
	EXPECT_TRUE(repository.read_record("account", "a").has_value());
}

TEST(rebuild_test, stops_rather_than_paging_for_ever_when_a_page_does_not_advance)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	// A page carrying nothing but the key it resumed at, and saying there is more, is a scan that
	// asking again would ask the same thing of for ever.
	nodes.answer_in_turn(peer, {
		tables({ "account" }),
		page({ "a" }, true),
		page({ "a" }, true)
	});

	EXPECT_EQ(0u, rebuild::rebuild(repository, nodes));
}

TEST(rebuild_test, asks_for_the_values_of_the_records_it_reads)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	nodes.answer_in_turn(peer, { tables({ "account" }), page({ "a" }, false) });

	rebuild::rebuild(repository, nodes, 25);

	const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();

	ASSERT_EQ(2u, sent.size());
	EXPECT_NE(std::string::npos, sent[1].second.query.find("values=true"));
	EXPECT_NE(std::string::npos, sent[1].second.query.find("limit=25"));
}

TEST(rebuild_test, resumes_from_a_key_that_has_to_be_encoded)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	nodes.answer_in_turn(peer, {
		tables({ "account" }),
		page({ "a", "a b/c" }, true),
		page({ "a b/c", "d" }, false)
	});

	EXPECT_EQ(3u, rebuild::rebuild(repository, nodes));

	const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();

	ASSERT_EQ(3u, sent.size());
	EXPECT_NE(std::string::npos, sent[2].second.query.find("from=" + url::encode("a b/c")));
}

// A node is out of the membership until its rebuild is done, so a rebuild that does not end is a
// copy the cluster waits for and never gets. The bound is on all of the round trips together,
// because each of them is bounded already and it is how many there are that is not.
TEST(rebuild_test, gives_up_when_it_runs_out_of_time)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, two_zones());

	nodes.answer_in_turn(peer, { tables({ "account" }), page({ "a", "b" }, false) });

	EXPECT_EQ(0u, rebuild::rebuild(repository, nodes, rebuild::default_page, 0));

	// Nothing was asked of the zone, rather than asked and thrown away.
	EXPECT_TRUE(nodes.sent().empty());
	EXPECT_FALSE(repository.has_table("account"));
}
