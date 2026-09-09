#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "base64/base64.h"
#include "cluster/partition.h"
#include "reconcile/reconcile.h"
#include "record/record.h"
#include "table/table.h"
#include "url/url.h"
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

	// The partitions the first file asked of a node named, which is the whole of what decides what
	// that node sends back.
	cluster::partition_set asked_for(const cluster::fake_cluster &nodes, const std::string &node)
	{
		const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();

		std::vector<std::pair<std::string, router::request>>::const_iterator asked = std::find_if(
			sent.begin(),
			sent.end(),
			[&node](const std::pair<std::string, router::request> &request)
			{
				return request.first == node &&
					request.second.path.size() == 3 && request.second.path[2] == "file";
			});

		if (asked == sent.end())
		{
			return cluster::partition_set();
		}

		return cluster::decode_partitions(url::read_parameter(asked->second.query, "partitions"))
			.value_or(cluster::partition_set());
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
	nodes.answer(mate, file({ "a" }));

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	EXPECT_EQ(1u, done.fetched);
	EXPECT_EQ("value of a", repository.read_record("account", "a").value_or(""));
}

TEST(reconcile_test, does_not_overwrite_a_record_it_holds_already)
{
	repository::fake_repository repository = store({ "a" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { self, peer, other });
	nodes.answer(mate, file({ "a" }));

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	// A local record is this node's own copy and the newer of the two: every write since the
	// ownership moved came here, and what the file carries was written before it did.
	EXPECT_EQ(0u, done.fetched);
	EXPECT_EQ("here already", repository.read_record("account", "a").value_or(""));
}

TEST(reconcile_test, does_not_fetch_a_record_it_does_not_own)
{
	repository::fake_repository repository = store({});
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { mate, peer, other });
	nodes.answer(mate, file({}));

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	EXPECT_EQ(0u, done.fetched);
	EXPECT_FALSE(asked_for(nodes, mate).test(cluster::partition_of("a")));
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
	nodes.answer(mate, file({}));
	nodes.answer(peer, file({}));
	nodes.answer(other, file({ "a" }));

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

	nodes.answer_in_turn(mate, { file({ "gained" }), holds() });

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

// A share is every record another node holds, and what a pass wants of it is the fraction whose
// owner moved. The file it asks for is what says which fraction, so nothing else crosses at all.
TEST(reconcile_test, asks_for_the_partitions_it_gained_and_for_no_others)
{
	repository::fake_repository repository = store({});
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("gained", { self, peer, other });
	nodes.copies("theirs", { mate, peer, other });

	nodes.answer(mate, file({ "gained" }));

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	cluster::partition_set wanted = asked_for(nodes, mate);

	EXPECT_TRUE(wanted.test(cluster::partition_of("gained")));
	EXPECT_FALSE(wanted.test(cluster::partition_of("theirs")));

	EXPECT_EQ(1u, done.fetched);
	EXPECT_EQ("value of gained", repository.read_record("account", "gained").value_or(""));
	EXPECT_FALSE(repository.read_record("account", "theirs").has_value());
}

// The fetch half asks for a file at a time, and a share larger than one is several of them, resumed
// from the key the walk that wrote the one before it reached.
TEST(reconcile_test, asks_for_the_next_file_from_the_key_the_one_before_it_reached)
{
	repository::fake_repository repository = store({});
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { self, peer, other });
	nodes.copies("b", { self, peer, other });

	nodes.answer_in_turn(mate, { file({ "a" }, "a"), file({ "b" }) });

	reconcile::outcome done = reconcile::reconcile(repository, nodes);

	EXPECT_EQ(2u, done.fetched);
	EXPECT_TRUE(repository.read_record("account", "a").has_value());
	EXPECT_TRUE(repository.read_record("account", "b").has_value());

	const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();

	ASSERT_LE(2u, sent.size());
	EXPECT_NE(std::string::npos, sent[1].second.query.find("from=" + url::encode(base64::encode("a"))));
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
