#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "base64/base64.h"
#include "cluster/partition.h"
#include "reconcile/reconcile.h"
#include "record/record.h"
#include "table/table.h"
#include "table/schema.h"
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

	// A pass that has not been told to stop, which is every pass but the one being shut down.
	const std::atomic<bool> running(true);

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
	router::response file(const std::vector<std::string> &keys, bool values, const std::string &next)
	{
		repository::fake_repository source;

		source.create_table(table::valid_table("account", std::vector<std::string>()), record::version { 1, 1 });

		for (size_t i = 0; i < keys.size(); i++)
		{
			source.write_record("account", record::valid_record(keys[i], "value of " + keys[i]));
		}

		repository::share whole;

		whole.partitions.set();
		whole.values = values;

		repository::extract taken = source.export_records("account", whole);

		return router::file_response(taken.file, taken.records, next.empty() ? "" : base64::encode(next));
	}

	// The schema as a node names it, which is the first thing a pass asks any node for: a table
	// this node is missing is one it would refuse every write to.
	router::response named(
		const std::vector<std::string> &names,
		const std::vector<std::string> &dropped = std::vector<std::string>(),
		const record::version &stamp = record::version { 1, 1 })
	{
		table::schema schema;

		for (size_t i = 0; i < names.size(); i++)
		{
			schema.create(table::valid_table(names[i], std::vector<std::string>()), stamp);
		}

		// A name the cluster dropped, at the version the delete was ordered in. It is what tells a
		// node holding that table that it missed the delete rather than that its peer missed the
		// create.
		for (size_t i = 0; i < dropped.size(); i++)
		{
			schema.remove(dropped[i], stamp);
		}

		return router::json_response(boost::beast::http::status::ok, schema.json());
	}

	// A file of records, which is what a node answers a fetch with.
	router::response records(const std::vector<std::string> &keys, const std::string &next = "")
	{
		return file(keys, true, next);
	}

	// A file of the keys alone, which is what the node that owns them answers a clear down with:
	// what is being asked is which of them it has, not what is in them.
	router::response holds(const std::vector<std::string> &keys, const std::string &next = "")
	{
		return file(keys, false, next);
	}

	record::record stamped(const std::string &key, const std::string &value, uint64_t term, uint64_t count)
	{
		record::record written = record::valid_record(key, value);

		written.stamp = record::version { term, count };

		return written;
	}

	// The same file the node being read from would have answered with, for keys written in a
	// version a test names rather than in none.
	router::response versioned_file(
		const std::vector<std::string> &keys,
		const std::string &value,
		bool values,
		uint64_t term,
		uint64_t count)
	{
		repository::fake_repository source;

		source.create_table(table::valid_table("account", std::vector<std::string>()), record::version { 1, 1 });

		for (size_t i = 0; i < keys.size(); i++)
		{
			source.write_record("account", stamped(keys[i], value, term, count));
		}

		repository::share whole;

		whole.partitions.set();
		whole.values = values;

		repository::extract taken = source.export_records("account", whole);

		return router::file_response(taken.file, taken.records, "");
	}

	repository::fake_repository store(const std::vector<std::string> &keys)
	{
		repository::fake_repository repository;

		repository.create_table(table::valid_table("account", std::vector<std::string>()), record::version { 1, 1 });

		for (size_t i = 0; i < keys.size(); i++)
		{
			repository.write_record("account", record::valid_record(keys[i], "here already"));
		}

		return repository;
	}

	// One worker, so that the order these tests read is the order they wrote. What several of them
	// do is transfer_test's to say.
	reconcile::outcome reconciled(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::atomic<bool> &flag,
		size_t page = reconcile::default_page,
		long seconds = reconcile::default_seconds)
	{
		return reconcile::reconcile(repository, nodes, flag, page, seconds, 1);
	}

	bool is_file(const router::request &request, bool values)
	{
		return request.path.size() == 3 && request.path[2] == "file" &&
			url::read_parameter(request.query, "values") == (values ? "true" : "false");
	}

	cluster::partition_set partitions_of(const router::request &request)
	{
		return cluster::decode_partitions(url::read_parameter(request.query, "partitions"))
			.value_or(cluster::partition_set());
	}

	// The partitions the first file of records asked of a node named, which is the whole of what
	// decides what that node sends back.
	cluster::partition_set asked_for(const cluster::fake_cluster &nodes, const std::string &node)
	{
		const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();

		std::vector<std::pair<std::string, router::request>>::const_iterator asked = std::find_if(
			sent.begin(),
			sent.end(),
			[&node](const std::pair<std::string, router::request> &request)
			{
				return request.first == node && is_file(request.second, true);
			});

		return asked == sent.end() ? cluster::partition_set() : partitions_of(asked->second);
	}

	// Where the question that lets this key go was put, which is the whole of what makes clearing
	// down safe.
	std::string asked_about(const cluster::fake_cluster &nodes, const std::string &key)
	{
		const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();

		std::vector<std::pair<std::string, router::request>>::const_iterator asked = std::find_if(
			sent.begin(),
			sent.end(),
			[&key](const std::pair<std::string, router::request> &request)
			{
				return is_file(request.second, false) &&
					partitions_of(request.second).test(cluster::partition_of(key));
			});

		return asked == sent.end() ? "" : asked->first;
	}

	// The queries of the files asked of a node, in the order they were asked for, so that a test
	// about resuming a walk does not also depend on which node a pass asks first.
	std::vector<std::string> file_queries_of(const cluster::fake_cluster &nodes, const std::string &node)
	{
		const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();
		std::vector<std::string> queries;

		for (size_t i = 0; i < sent.size(); i++)
		{
			if (sent[i].first == node && is_file(sent[i].second, true))
			{
				queries.push_back(sent[i].second.query);
			}
		}

		return queries;
	}

	size_t files_asked_of(const cluster::fake_cluster &nodes, const std::string &node, bool values)
	{
		const std::vector<std::pair<std::string, router::request>> &sent = nodes.sent();

		return static_cast<size_t>(std::count_if(
			sent.begin(),
			sent.end(),
			[&node, values](const std::pair<std::string, router::request> &request)
			{
				return request.first == node && is_file(request.second, values);
			}));
	}
}

TEST(reconcile_test, moves_nothing_for_an_instance_standing_alone)
{
	repository::fake_repository repository = store({ "a" });
	cluster::fake_cluster nodes(self, std::vector<cluster::member>());

	reconcile::outcome done = reconciled(repository, nodes, running);

	EXPECT_EQ(0u, done.fetched);
	EXPECT_EQ(0u, done.cleared);
	EXPECT_EQ(0u, done.deferred);
	EXPECT_TRUE(done.settled());
	EXPECT_FALSE(done.moved());

	// Nobody was asked anything, and the record it owns on its own is still there.
	EXPECT_TRUE(nodes.sent().empty());
	EXPECT_TRUE(repository.read_record("account", "a").has_value());
}

// A pass moving a share of a terabyte is not one a node being shut down waits out, so it is told
// to stop rather than kept short enough not to matter.
TEST(reconcile_test, a_pass_that_is_no_longer_running_asks_nobody_anything)
{
	repository::fake_repository repository = store({ "gone" });
	cluster::fake_cluster nodes(self, three_zones());
	const std::atomic<bool> stopped(false);

	nodes.copies("gone", { mate, peer, other });
	nodes.answer(mate, holds({ "gone" }));

	reconcile::outcome done = reconciled(repository, nodes, stopped);

	EXPECT_EQ(0u, done.cleared);
	EXPECT_FALSE(done.settled());
	EXPECT_TRUE(nodes.sent().empty());
	EXPECT_TRUE(repository.read_record("account", "gone").has_value());
}

TEST(reconcile_test, fetches_a_record_this_node_owns_and_holds_nothing_for)
{
	repository::fake_repository repository = store({});
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { self, peer, other });
	nodes.answer(mate, records({ "a" }));

	reconcile::outcome done = reconciled(repository, nodes, running);

	EXPECT_EQ(1u, done.fetched);
	EXPECT_TRUE(done.moved());
	EXPECT_EQ("value of a", repository.read_record("account", "a").value_or(""));
}

TEST(reconcile_test, does_not_overwrite_a_record_it_holds_already)
{
	repository::fake_repository repository = store({ "a" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { self, peer, other });
	nodes.answer(mate, records({ "a" }));

	reconcile::outcome done = reconciled(repository, nodes, running);

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
	nodes.answer(mate, records({}));

	reconcile::outcome done = reconciled(repository, nodes, running);

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
	nodes.answer(mate, records({}));
	nodes.answer(peer, records({}));
	nodes.answer(other, records({ "a" }));

	reconcile::outcome done = reconciled(repository, nodes, running);

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
	nodes.answer_in_turn(mate, { named({ "account" }), records({}), holds({ "a" }) });

	// The fetch half asks every node of every zone, and a pass any of them would not answer is a
	// pass that is not settled whatever the clear down did.
	nodes.answer(peer, records({}));
	nodes.answer(other, records({}));

	reconcile::outcome done = reconciled(repository, nodes, running);

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
	nodes.answer(mate, holds({}));

	reconcile::outcome done = reconciled(repository, nodes, running);

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
	nodes.answer(mate, holds({}));
	nodes.answer(peer, holds({ "a" }));
	nodes.answer(other, holds({ "a" }));

	reconcile::outcome done = reconciled(repository, nodes, running);

	EXPECT_EQ(mate, asked_about(nodes, "a"));
	EXPECT_EQ(0u, done.cleared);
	EXPECT_TRUE(repository.read_record("account", "a").has_value());
}

// The whole point of asking with a file. A share is given up on one answer from the node that owns
// it, where a question for every key is a round trip for every record a membership change moved.
TEST(reconcile_test, asks_the_owner_once_for_a_share_rather_than_once_for_every_record)
{
	repository::fake_repository repository = store({ "a", "b", "c", "d", "e" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { mate, peer, other });
	nodes.copies("b", { mate, peer, other });
	nodes.copies("c", { mate, peer, other });
	nodes.copies("d", { mate, peer, other });
	nodes.copies("e", { mate, peer, other });

	nodes.answer_in_turn(mate, { named({ "account" }), records({}), holds({ "a", "b", "c", "d", "e" }) });

	reconcile::outcome done = reconciled(repository, nodes, running);

	EXPECT_EQ(5u, done.cleared);
	EXPECT_EQ(1u, files_asked_of(nodes, mate, false));
}

// And the answer carries keys and no values, because what is being decided is where a record
// belongs and not what is in it.
TEST(reconcile_test, asks_the_owner_for_keys_and_never_for_values)
{
	repository::fake_repository repository = store({ "a" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { mate, peer, other });
	nodes.answer(mate, holds({ "a" }));

	reconciled(repository, nodes, running);

	EXPECT_EQ(1u, files_asked_of(nodes, mate, false));
}

TEST(reconcile_test, keeps_a_record_there_is_nobody_to_ask_about)
{
	repository::fake_repository repository = store({ "a" });
	cluster::fake_cluster nodes(self, three_zones());

	// A key this node holds no copy of and owns no copy of is one nothing can be asked about.
	nodes.copies("a", std::vector<std::string>());

	reconcile::outcome done = reconciled(repository, nodes, running);

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

	nodes.answer_in_turn(mate, { named({ "account" }), records({ "gained" }), holds({ "gone" }) });
	nodes.answer(peer, records({}));
	nodes.answer(other, records({}));

	reconcile::outcome done = reconciled(repository, nodes, running);

	EXPECT_EQ(1u, done.fetched);
	EXPECT_EQ(1u, done.cleared);
	EXPECT_TRUE(done.settled());

	EXPECT_EQ("value of gained", repository.read_record("account", "gained").value_or(""));
	EXPECT_FALSE(repository.read_record("account", "gone").has_value());
}

// A pass that ran out of patience has done none of what it did not reach, and a half it never
// walked defers nothing to say so — so the counts of a truncated pass are the counts of a settled
// one, and the difference between them is the only thing that makes the pass after it run.
TEST(reconcile_test, a_pass_that_ran_out_of_patience_is_not_settled)
{
	repository::fake_repository repository = store({ "gone" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("gone", { mate, peer, other });
	nodes.answer(mate, holds({ "gone" }));

	reconcile::outcome done = reconciled(repository, nodes, running, reconcile::default_page, 0);

	EXPECT_EQ(0u, done.cleared);
	EXPECT_EQ(0u, done.deferred);
	EXPECT_FALSE(done.settled());
	EXPECT_TRUE(repository.read_record("account", "gone").has_value());
}

// A node deaf to its peers and still renewing its lease is in the membership and answers nothing,
// so a share it holds is a share this pass did not take. Saying so is what makes the pass after
// this one run: the membership has not moved, and nothing else buys the attempts to ask again.
TEST(reconcile_test, a_share_the_node_holding_it_would_not_answer_for_is_not_settled)
{
	repository::fake_repository repository = store({});
	cluster::fake_cluster nodes(self, three_zones());

	// This node owns the key and holds nothing for it, and no node is told what to answer, so
	// every node it asks refuses the file.
	nodes.copies("a", { self, peer, other });

	reconcile::outcome done = reconciled(repository, nodes, running);

	EXPECT_EQ(0u, done.fetched);
	EXPECT_TRUE(done.refused);
	EXPECT_FALSE(done.settled());
	EXPECT_FALSE(repository.read_record("account", "a").has_value());
}

// The halves are not each other's precondition, so a node that would not answer costs the pass
// what that node holds and nothing else: what the owner in this zone did answer for is still safe
// to give up.
TEST(reconcile_test, a_refused_fetch_does_not_stop_the_clear_down)
{
	repository::fake_repository repository = store({ "gone" });
	cluster::fake_cluster nodes(self, three_zones());

	// The owner in this node's own zone answers, and the two further zones answer nothing.
	nodes.copies("gone", { mate, peer, other });
	nodes.answer_in_turn(mate, { named({ "account" }), records({}), holds({ "gone" }) });

	reconcile::outcome done = reconciled(repository, nodes, running);

	EXPECT_EQ(1u, done.cleared);
	EXPECT_TRUE(done.refused);
	EXPECT_FALSE(done.settled());
	EXPECT_FALSE(repository.read_record("account", "gone").has_value());
}

// Patience is not a deadline: a half still being answered goes on being answered, which is what
// lets one pass move a share rather than the pass after it starting the same share again.
TEST(reconcile_test, keeps_going_while_the_files_keep_arriving)
{
	repository::fake_repository repository = store({});
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { self, peer, other });
	nodes.copies("b", { self, peer, other });

	// Two files, each of them longer than the whole patience of the half that asks for them.
	nodes.answer_in_turn(mate, { named({ "account" }), records({ "a" }, "a"), records({ "b" }) });
	nodes.slow(mate, std::chrono::milliseconds(700));

	reconcile::outcome done = reconciled(repository, nodes, running, reconcile::default_page, 1);

	EXPECT_EQ(2u, done.fetched);
	EXPECT_TRUE(repository.read_record("account", "a").has_value());
	EXPECT_TRUE(repository.read_record("account", "b").has_value());
}

// The clear down has patience of its own, and it is what a tier that has just grown needs: the
// fetch walks every node of every zone, and a store of large values is one it spends a long time
// in. A pass that spent longer than its own patience fetching still clears down.
TEST(reconcile_test, clears_down_what_it_lost_after_a_fetch_that_took_a_long_time)
{
	repository::fake_repository repository = store({ "gone" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("gone", { mate, peer, other });
	nodes.answer_in_turn(mate, { named({ "account" }), records({}), holds({ "gone" }) });

	// Longer than the patience of either half, so a clock the two halves shared would be spent
	// before the clear down began.
	nodes.slow(peer, std::chrono::milliseconds(1100));

	reconcile::outcome done = reconciled(repository, nodes, running, reconcile::default_page, 1);

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

	nodes.answer(mate, records({ "gained" }));

	reconcile::outcome done = reconciled(repository, nodes, running);

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

	nodes.answer_in_turn(mate, { named({ "account" }), records({ "a" }, "a"), records({ "b" }) });

	reconcile::outcome done = reconciled(repository, nodes, running);

	EXPECT_EQ(2u, done.fetched);
	EXPECT_TRUE(repository.read_record("account", "a").has_value());
	EXPECT_TRUE(repository.read_record("account", "b").has_value());

	std::vector<std::string> asked = file_queries_of(nodes, mate);

	ASSERT_EQ(2u, asked.size());
	EXPECT_EQ(std::string::npos, asked[0].find("from="));
	EXPECT_NE(std::string::npos, asked[1].find("from=" + url::encode(base64::encode("a"))));
}

TEST(reconcile_test, walks_a_store_larger_than_one_page)
{
	repository::fake_repository repository = store({ "a", "b", "c" });
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { mate, peer, other });
	nodes.copies("b", { mate, peer, other });
	nodes.copies("c", { mate, peer, other });
	nodes.answer_in_turn(mate, { named({ "account" }), records({}), holds({ "a", "b", "c" }) });

	reconcile::outcome done = reconciled(repository, nodes, running, 1);

	EXPECT_EQ(3u, done.cleared);
	EXPECT_FALSE(repository.read_record("account", "a").has_value());
	EXPECT_FALSE(repository.read_record("account", "b").has_value());
	EXPECT_FALSE(repository.read_record("account", "c").has_value());
}

// The gap a version closes. This node was not there for the write the other copies took, so what it
// holds is the older of two records — and it is the owner of the key, so nothing about where the
// record belongs is wrong and the clear down has nothing to say about it. The fetch is what catches
// it up.
TEST(reconcile_test, takes_a_record_written_after_the_one_this_node_holds)
{
	repository::fake_repository repository;

	repository.create_table(table::valid_table("account", std::vector<std::string>()), record::version { 1, 1 });
	repository.write_record("account", stamped("a", "stale", 41, 4));

	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { self, peer, other });
	nodes.answer(peer, versioned_file({ "a" }, "fresh", true, 41, 9));

	reconcile::outcome done = reconciled(repository, nodes, running, reconcile::default_page, 1);

	EXPECT_EQ(1u, done.fetched);
	EXPECT_EQ("fresh", repository.read_record("account", "a").value_or(""));
}

// And the write this node was there for is not undone by a copy that was not: a record held here
// at a later version is one the file cannot touch.
TEST(reconcile_test, keeps_a_record_written_after_the_one_a_file_carries)
{
	repository::fake_repository repository;

	repository.create_table(table::valid_table("account", std::vector<std::string>()), record::version { 1, 1 });
	repository.write_record("account", stamped("a", "fresh", 41, 9));

	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { self, peer, other });
	nodes.answer(peer, versioned_file({ "a" }, "stale", true, 41, 4));

	reconcile::outcome done = reconciled(repository, nodes, running, reconcile::default_page, 1);

	EXPECT_EQ(0u, done.fetched);
	EXPECT_EQ("fresh", repository.read_record("account", "a").value_or(""));
}

// Clearing down is where a version stops a write being lost rather than merely staling. The node
// that owns this key now holds an older copy of it, so handing it over and deleting this one would
// take the later write out of the zone altogether. It is kept, and the pass does not settle until
// the owner has caught up.
TEST(reconcile_test, keeps_a_record_the_node_that_owns_it_has_yet_to_catch_up_on)
{
	repository::fake_repository repository;

	repository.create_table(table::valid_table("account", std::vector<std::string>()), record::version { 1, 1 });
	repository.write_record("account", stamped("gone", "fresh", 41, 9));

	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("gone", { mate, peer, other });
	nodes.answer(mate, versioned_file({ "gone" }, "stale", false, 41, 4));

	reconcile::outcome done = reconciled(repository, nodes, running, reconcile::default_page, 1);

	EXPECT_EQ(0u, done.cleared);
	EXPECT_EQ(1u, done.deferred);
	EXPECT_FALSE(done.settled());
	EXPECT_EQ("fresh", repository.read_record("account", "gone").value_or(""));
}

// The owner has caught up, so this copy is the one to go: the record is in the zone either way and
// the node that owns it is the one that should be holding it.
TEST(reconcile_test, gives_up_a_record_the_node_that_owns_it_holds_at_the_same_version)
{
	repository::fake_repository repository;

	repository.create_table(table::valid_table("account", std::vector<std::string>()), record::version { 1, 1 });
	repository.write_record("account", stamped("gone", "the write", 41, 9));

	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("gone", { mate, peer, other });
	nodes.answer(mate, versioned_file({ "gone" }, "the write", false, 41, 9));

	reconcile::outcome done = reconciled(repository, nodes, running, reconcile::default_page, 1);

	EXPECT_EQ(1u, done.cleared);
	EXPECT_FALSE(repository.read_record("account", "gone").has_value());
}

// Nothing else in the cluster puts a table on a node that missed the create: the rebuild that
// copies them runs on an empty store alone, and a node that came back to one it was left with
// would refuse every write to that table for the keys it owns.
TEST(reconcile_test, declares_a_table_the_rest_of_the_cluster_has)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, three_zones());

	nodes.answer(mate, named({ "account" }));

	reconciled(repository, nodes, running);

	EXPECT_TRUE(repository.has_table("account"));
}

// And it is declared before the tables are read, so the records of a table this pass learned about
// are the records this pass fetches.
TEST(reconcile_test, fills_a_table_it_declared_in_the_same_pass)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, three_zones());

	nodes.copies("a", { self, peer, other });
	nodes.answer_in_turn(mate, { named({ "account" }), records({ "a" }) });

	reconcile::outcome done = reconciled(repository, nodes, running);

	EXPECT_EQ(1u, done.fetched);
	EXPECT_EQ("value of a", repository.read_record("account", "a").value_or(""));
}

// A name no other node mentions at all is a node that is wrong about the schema rather than a
// delete this node missed, and dropping a table takes its records with it — so it is left standing.
// It is the tombstone below, and nothing else, that says a name is gone.
TEST(reconcile_test, keeps_a_table_no_other_node_names)
{
	repository::fake_repository repository = store({});

	repository.create_table(table::valid_table("kept", std::vector<std::string>()), record::version { 1, 1 });

	cluster::fake_cluster nodes(self, three_zones());

	nodes.answer(mate, named({ "account" }));

	reconciled(repository, nodes, running);

	EXPECT_TRUE(repository.has_table("kept"));
}

// The delete this node missed, as the rest of the cluster carries it: a name it holds live against
// a tombstone stamped later. That is the one thing that drops a table here, and it takes the
// records with it as a delete always does.
TEST(reconcile_test, drops_a_table_the_cluster_dropped_while_this_node_was_away)
{
	repository::fake_repository repository = store({});

	repository.create_table(table::valid_table("gone", std::vector<std::string>()), record::version { 1, 1 });
	repository.write_record("gone", record::valid_record("a", "a value"));

	cluster::fake_cluster nodes(self, three_zones());

	nodes.answer(mate, named({ "account" }, { "gone" }, record::version { 1, 2 }));

	reconciled(repository, nodes, running);

	EXPECT_FALSE(repository.has_table("gone"));
	EXPECT_FALSE(repository.read_record("gone", "a").has_value());
}

// A tombstone older than the create this node holds is a node that has not caught up with the
// table being made again, and acting on it would drop a table the cluster has.
TEST(reconcile_test, keeps_a_table_against_a_tombstone_older_than_it)
{
	repository::fake_repository repository = store({});

	repository.create_table(table::valid_table("kept", std::vector<std::string>()), record::version { 2, 1 });

	cluster::fake_cluster nodes(self, three_zones());

	nodes.answer(mate, named({}, { "kept" }, record::version { 1, 9 }));

	reconciled(repository, nodes, running);

	EXPECT_TRUE(repository.has_table("kept"));
}

// **A live entry never drops a column family.** One create carried to a node that had missed it is
// stamped again, so two nodes can hold one table at two versions — and taking the later document
// is right where taking the later table would throw the records away.
TEST(reconcile_test, keeps_the_records_of_a_table_the_cluster_stamped_again)
{
	repository::fake_repository repository = store({});

	repository.create_table(table::valid_table("account", std::vector<std::string>()), record::version { 1, 1 });
	repository.write_record("account", record::valid_record("a", "a value"));

	cluster::fake_cluster nodes(self, three_zones());

	nodes.answer(mate, named({ "account" }, {}, record::version { 9, 9 }));

	reconciled(repository, nodes, running);

	EXPECT_TRUE(repository.has_table("account"));
	EXPECT_EQ("a value", repository.read_record("account", "a").value_or(""));
}

// A pass that has been told to stop asks nobody for anything, the schema included.
TEST(reconcile_test, a_pass_that_is_no_longer_running_asks_for_no_tables)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes(self, three_zones());
	const std::atomic<bool> stopped(false);

	nodes.answer(mate, named({ "account" }));

	reconciled(repository, nodes, stopped);

	EXPECT_FALSE(repository.has_table("account"));
	EXPECT_TRUE(nodes.sent().empty());
}
