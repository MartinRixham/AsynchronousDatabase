#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <boost/json.hpp>

#include "cluster/cluster.h"
#include "reconcile/reconcile.h"
#include "record/record.h"
#include "table/table.h"
#include "../cluster/fake_cluster.h"
#include "../repository/fake_repository.h"

namespace
{
	const std::string self = "http://one:8080";

	// The other node of this node's own zone, which is where a partition this node gives up goes
	// and where one it takes over comes from.
	const std::string mate = "http://two:8080";

	const std::string peer = "http://three:8080";

	const std::string other = "http://four:8080";

	// Two nodes in this node's zone, and one in each of two more.
	std::vector<cluster::member> three_zones()
	{
		return std::vector<cluster::member> {
			cluster::member { self, "one" },
			cluster::member { mate, "one" },
			cluster::member { peer, "two" },
			cluster::member { other, "three" }
		};
	}

	router::response page(const std::vector<std::string> &keys)
	{
		boost::json::array records;

		for (size_t i = 0; i < keys.size(); i++)
		{
			records.push_back(boost::json::object {
				{ "key", boost::json::string(keys[i]) },
				{ "value", boost::json::string("value of " + keys[i]) }
			});
		}

		return router::json_response(
			boost::beast::http::status::ok, boost::json::object { { "records", records } });
	}

	// A node that holds the key it is asked for, and an answer a scan of it reads nothing out of:
	// what is being told apart is a HEAD of one record, not a page.
	router::response holds()
	{
		return router::empty_response(boost::beast::http::status::ok);
	}

	router::response holds_nothing()
	{
		return router::empty_response(boost::beast::http::status::not_found);
	}

	repository::fake_repository store(const std::vector<std::string> &keys)
	{
		repository::fake_repository repository;

		repository.create_table(table::valid_table("account", std::vector<std::string>()));

		for (size_t i = 0; i < keys.size(); i++)
		{
			repository.write_record("account", record::valid_record(keys[i], "here already"));
		}

		return repository;
	}

	// Where a HEAD of this key went, which is the whole of what makes clearing down safe.
	std::string asked_about(const cluster::fake_cluster &nodes, const std::string &key)
	{
		const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();

		std::vector<std::pair<std::string, router::request>>::const_iterator asked = std::find_if(
			sent.begin(),
			sent.end(),
			[&key](const std::pair<std::string, router::request> &request)
			{
				return request.second.method == boost::beast::http::verb::head &&
					request.second.path.size() == 4 && request.second.path[3] == key;
			});

		return asked == sent.end() ? "" : asked->first;
	}
}

TEST(reconcile_test, moves_nothing_for_an_instance_standing_alone)
{
	repository::fake_repository repository = store({ "a" });
	cluster::fake_cluster nodes(self, std::vector<cluster::member>());

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	EXPECT_EQ(0u, done.fetched);
	EXPECT_EQ(0u, done.cleared);
	EXPECT_EQ(0u, done.deferred);
	EXPECT_TRUE(done.settled());

	// Nobody was asked anything, and the record it owns on its own is still there.
	EXPECT_TRUE(nodes.sent().empty());
	EXPECT_TRUE(repository.read_record("account", "a").has_value());
}

TEST(reconcile_test, fetches_a_record_this_node_owns_and_holds_nothing_for)
{
	repository::fake_repository repository = store({});
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { self, peer, other });
	nodes.answer(mate, page({ "a" }));

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	EXPECT_EQ(1u, done.fetched);
	EXPECT_EQ("value of a", repository.read_record("account", "a").value_or(""));
}

TEST(reconcile_test, does_not_overwrite_a_record_it_holds_already)
{
	repository::fake_repository repository = store({ "a" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { self, peer, other });
	nodes.answer(mate, page({ "a" }));

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	// A local record is this node's own copy and as current as any: a write reaches every copy, so
	// another node's answer is the same value or an older one.
	EXPECT_EQ(0u, done.fetched);
	EXPECT_EQ("here already", repository.read_record("account", "a").value_or(""));
}

TEST(reconcile_test, does_not_fetch_a_record_it_does_not_own)
{
	repository::fake_repository repository = store({});
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { mate, peer, other });
	nodes.answer(mate, page({ "a" }));

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	EXPECT_EQ(0u, done.fetched);
	EXPECT_FALSE(repository.read_record("account", "a").has_value());
}

// The one that makes a fetch a repair rather than a copy of one zone. A record written while a zone
// could not be reached is in one zone only, so a pass that stopped at the first zone to answer
// would leave a copy short and call it settled.
TEST(reconcile_test, fetches_from_a_further_zone_when_the_nearer_ones_hold_nothing)
{
	repository::fake_repository repository = store({});
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { self, peer, other });
	nodes.answer(mate, page({}));
	nodes.answer(peer, page({}));
	nodes.answer(other, page({ "a" }));

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	EXPECT_EQ(1u, done.fetched);
	EXPECT_EQ("value of a", repository.read_record("account", "a").value_or(""));
}

TEST(reconcile_test, clears_down_a_record_the_owner_in_its_own_zone_holds)
{
	repository::fake_repository repository = store({ "a" });
	cluster::fake_cluster nodes(self, three_zones());

	// The copy in this node's own zone is the first this node would ask, which is how replicas
	// orders them.
	nodes.copies("a", { mate, peer, other });
	nodes.answer(mate, holds());

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	EXPECT_EQ(1u, done.cleared);
	EXPECT_EQ(0u, done.deferred);
	EXPECT_TRUE(done.settled());
	EXPECT_FALSE(repository.read_record("account", "a").has_value());
}

TEST(reconcile_test, keeps_a_record_the_owner_in_its_own_zone_has_not_taken_over_yet)
{
	repository::fake_repository repository = store({ "a" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { mate, peer, other });
	nodes.answer(mate, holds_nothing());

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	// The copy is never the last one: a node that has not fetched what it now owns is a node this
	// one waits for, and a pass that is waiting is not settled.
	EXPECT_EQ(0u, done.cleared);
	EXPECT_EQ(1u, done.deferred);
	EXPECT_FALSE(done.settled());
	EXPECT_TRUE(repository.read_record("account", "a").has_value());
}

// Asking any copy would be a zone left holding nothing: the record here is this zone's copy, and
// another zone still having one says nothing about that.
TEST(reconcile_test, asks_the_owner_in_its_own_zone_and_never_another_zone)
{
	repository::fake_repository repository = store({ "a" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { mate, peer, other });
	nodes.answer(mate, holds_nothing());
	nodes.answer(peer, holds());
	nodes.answer(other, holds());

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	EXPECT_EQ(mate, asked_about(nodes, "a"));
	EXPECT_EQ(0u, done.cleared);
	EXPECT_TRUE(repository.read_record("account", "a").has_value());
}

TEST(reconcile_test, keeps_a_record_there_is_nobody_to_ask_about)
{
	repository::fake_repository repository = store({ "a" });
	cluster::fake_cluster nodes(self, three_zones());

	// A key this node holds no copy of and owns no copy of is one nothing can be asked about.
	nodes.copies("a", std::vector<std::string>());

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	EXPECT_EQ(0u, done.cleared);
	EXPECT_EQ(1u, done.deferred);
	EXPECT_TRUE(repository.read_record("account", "a").has_value());
}

// Both halves in one pass, which is the order the cluster converges in: the node that gained the
// partition holds it before the node that lost one asks whether it does.
TEST(reconcile_test, fetches_what_it_gained_and_clears_down_what_it_lost)
{
	repository::fake_repository repository = store({ "gone" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("gained", { self, peer, other });
	nodes.copies("gone", { mate, peer, other });

	nodes.answer_in_turn(mate, { page({ "gained" }), holds() });

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	EXPECT_EQ(1u, done.fetched);
	EXPECT_EQ(1u, done.cleared);
	EXPECT_TRUE(done.settled());

	EXPECT_EQ("value of gained", repository.read_record("account", "gained").value_or(""));
	EXPECT_FALSE(repository.read_record("account", "gone").has_value());
}

// A pass its own clock ended has done none of what it did not reach, and a half it never walked
// defers nothing to say so — so the counts of a truncated pass are the counts of a settled one,
// and the difference between them is the only thing that makes the pass after it run.
TEST(reconcile_test, a_pass_its_own_clock_ended_is_not_settled)
{
	repository::fake_repository repository = store({ "gone" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("gone", { mate, peer, other });
	nodes.answer(mate, holds());

	reconcile::outcome done = reconcile::reconcile(repository, nodes, reconcile::default_page, 0);

	EXPECT_EQ(0u, done.cleared);
	EXPECT_EQ(0u, done.deferred);
	EXPECT_FALSE(done.settled());
	EXPECT_TRUE(repository.read_record("account", "gone").has_value());
}

// The clear down has a budget of its own, and it is what a tier that has just grown needs: the
// fetch walks every node of every zone, and a store of large values is one it never gets to the
// end of. A pass that spent longer than the whole of itself fetching still clears down.
TEST(reconcile_test, clears_down_what_it_lost_when_the_fetch_ran_out_of_time)
{
	repository::fake_repository repository = store({ "gone" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("gone", { mate, peer, other });
	nodes.answer(mate, holds());

	// Longer than the whole pass, so a deadline the two halves shared would be spent before the
	// clear down began.
	nodes.slow(peer, std::chrono::milliseconds(1100));

	reconcile::outcome done = reconcile::reconcile(repository, nodes, reconcile::default_page, 1);

	EXPECT_EQ(1u, done.cleared);
	EXPECT_FALSE(repository.read_record("account", "gone").has_value());
}

TEST(reconcile_test, walks_a_store_larger_than_one_page)
{
	repository::fake_repository repository = store({ "a", "b", "c" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { mate, peer, other });
	nodes.copies("b", { mate, peer, other });
	nodes.copies("c", { mate, peer, other });
	nodes.answer(mate, holds());

	reconcile::outcome done = reconcile::reconcile(repository, nodes, 1);

	EXPECT_EQ(3u, done.cleared);
	EXPECT_FALSE(repository.read_record("account", "a").has_value());
	EXPECT_FALSE(repository.read_record("account", "b").has_value());
	EXPECT_FALSE(repository.read_record("account", "c").has_value());
}
