#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "cluster/cluster.h"
#include "cluster/partition.h"
#include "progress/patience.h"
#include "record/record.h"
#include "repository/repository.h"
#include "router/router.h"
#include "table/table.h"
#include "transfer/transfer.h"
#include "../cluster/fake_cluster.h"
#include "../repository/fake_repository.h"

namespace
{
	const std::string here = "http://one:8080";

	const std::string there = "http://two:8080";

	const std::atomic<bool> running(true);

	// The node being read from, with a real router over a real store behind it — which is what a
	// walk is reading when it runs, rather than answers written out by hand. Only the two calls a
	// walk makes are answered; everything else is a cluster of one.
	class serving_node : public cluster::cluster
	{
		router::router &served;

		mutable std::atomic<size_t> asks = 0;

		mutable std::atomic<size_t> widest_fan_out = 0;

	public:
		explicit serving_node(router::router &router): served(router)
		{
		}

		size_t asked() const
		{
			return asks;
		}

		// The most requests this node was ever handed in one go, which is how many pieces of the
		// share were being read at the same time.
		size_t widest() const
		{
			return widest_fan_out;
		}

		router::response send(const std::string &, const router::request &request) const override
		{
			asks++;

			return served.route(request);
		}

		void start() override
		{
		}

		bool discover() override
		{
			return false;
		}

		void stop() override
		{
		}

		std::vector<::cluster::member> members() const override
		{
			return std::vector<::cluster::member>();
		}

		::cluster::placement replicas(const std::string &) const override
		{
			return ::cluster::placement();
		}

		::cluster::placement copies_of(size_t) const override
		{
			return ::cluster::placement();
		}

		::cluster::partition_set holdings() const override
		{
			::cluster::partition_set held;

			held.set();

			return held;
		}

		std::map<std::string, ::cluster::partition_set> holders(
			const ::cluster::partition_set &) const override
		{
			return std::map<std::string, ::cluster::partition_set>();
		}

		std::map<std::string, ::cluster::partition_set> holders_in(
			const ::cluster::partition_set &,
			const std::vector<std::string> &) const override
		{
			return std::map<std::string, ::cluster::partition_set>();
		}

		std::vector<std::string> peers() const override
		{
			return std::vector<std::string>();
		}

		std::vector<std::vector<std::string>> zones() const override
		{
			return std::vector<std::vector<std::string>>();
		}

		std::optional<::cluster::leadership> leader(const std::string &) const override
		{
			return std::nullopt;
		}

		size_t leads() const override
		{
			return 0;
		}

		bool is_unled() const override
		{
			return false;
		}

		::cluster::etcd_registration registration() const override
		{
			return ::cluster::etcd_registration();
		}

		bool accept(const std::string &, int64_t) override
		{
			return true;
		}

		std::optional<router::response> send_all(
			const std::vector<std::string> &,
			const router::request &) const override
		{
			return std::nullopt;
		}

		std::vector<router::response> send_each(const std::vector<::cluster::enquiry> &enquiries) const override
		{
			std::vector<router::response> responses;

			if (enquiries.size() > widest_fan_out)
			{
				widest_fan_out = enquiries.size();
			}

			for (size_t i = 0; i < enquiries.size(); i++)
			{
				responses.push_back(send(enquiries[i].node, enquiries[i].request));
			}

			return responses;
		}
	};

	std::string key(size_t number)
	{
		return "account/" + std::to_string(1000 + number);
	}

	void fill(repository::fake_repository &repository, size_t records)
	{
		repository.create_table(table::valid_table("account", std::vector<std::string>()), record::version { 1, 1 });

		for (size_t i = 0; i < records; i++)
		{
			repository.write_record("account", record::valid_record(key(i), "value of " + key(i)));
		}
	}

	transfer::share share_of(size_t workers, size_t bytes)
	{
		transfer::share wanted;

		wanted.node = there;
		wanted.table = "account";
		wanted.partitions.set();
		wanted.workers = workers;
		wanted.bytes = bytes;

		return wanted;
	}

	// The keys of every file a walk was handed, read back out of a store of this node's own.
	std::set<std::string> taken_by(repository::fake_repository &taking, const std::vector<std::string> &files)
	{
		std::set<std::string> keys;

		for (size_t i = 0; i < files.size(); i++)
		{
			taking.import_records("account", files[i]);
		}

		// A scan reads one partition, so every key of the table is every partition asked in turn.
		for (size_t partition = 0; partition < cluster::partition_count; partition++)
		{
			scan::range whole;

			whole.is_valid = true;
			whole.partition = partition;
			whole.limit = scan::max_limit;

			scan::page walked = taking.scan_records("account", whole);

			for (size_t i = 0; i < walked.records.size(); i++)
			{
				keys.insert(walked.records[i].key);
			}
		}

		return keys;
	}

	// Every file a walk handed over, in whatever order the workers handed them.
	class collector
	{
		mutable std::mutex mutex;

		std::vector<std::string> files;

	public:
		void operator()(const std::string &file)
		{
			std::lock_guard<std::mutex> lock(mutex);

			files.push_back(file);
		}

		std::vector<std::string> taken() const
		{
			std::lock_guard<std::mutex> lock(mutex);

			return files;
		}
	};

	bool asked_at_least(const serving_node &node, size_t asks)
	{
		for (size_t i = 0; i < 200 && node.asked() < asks; i++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}

		return node.asked() >= asks;
	}
}

TEST(transfer_test, walks_a_share_in_one_piece_for_one_worker)
{
	repository::fake_repository store;
	repository::fake_repository taking;
	cluster::fake_cluster alone(there, std::vector<std::string>());
	router::router router(store, alone);
	serving_node node(router);
	progress::patience waiting(30);
	collector taken;

	fill(store, 8);
	taking.create_table(table::valid_table("account", std::vector<std::string>()), record::version { 1, 1 });

	transfer::outcome done = transfer::walk(node, share_of(1, 4096), running, waiting, std::ref(taken));

	EXPECT_TRUE(done.whole);
	EXPECT_FALSE(done.refused);
	EXPECT_EQ(8u, taken_by(taking, taken.taken()).size());

	// One piece is one walk, so nothing was asked about cutting the table up.
	EXPECT_EQ(1u, taken.taken().size());
	EXPECT_EQ(1u, node.asked());
}

// The whole point of several workers: the table is cut into a piece each and the pieces are read
// at the same time, so a share moves at more than the speed of one request at a time.
TEST(transfer_test, cuts_a_share_into_a_piece_for_every_worker)
{
	repository::fake_repository store;
	repository::fake_repository taking;
	cluster::fake_cluster alone(there, std::vector<std::string>());
	router::router router(store, alone);
	serving_node node(router);
	progress::patience waiting(30);
	collector taken;

	fill(store, 8);
	taking.create_table(table::valid_table("account", std::vector<std::string>()), record::version { 1, 1 });

	transfer::outcome done = transfer::walk(node, share_of(4, 4096), running, waiting, std::ref(taken));

	EXPECT_TRUE(done.whole);

	// One question about where to cut it up, and then a file for each of the four pieces.
	EXPECT_EQ(5u, node.asked());
	EXPECT_EQ(4u, taken.taken().size());

	// And the four were asked for together rather than one after another, which is the whole of
	// what makes them faster than one piece.
	EXPECT_EQ(4u, node.widest());
}

// And the pieces are a cover: every record crosses, and no record crosses twice.
TEST(transfer_test, takes_every_record_of_a_share_exactly_once)
{
	repository::fake_repository store;
	repository::fake_repository taking;
	cluster::fake_cluster alone(there, std::vector<std::string>());
	router::router router(store, alone);
	serving_node node(router);
	progress::patience waiting(30);
	collector taken;

	fill(store, 40);
	taking.create_table(table::valid_table("account", std::vector<std::string>()), record::version { 1, 1 });

	transfer::outcome done = transfer::walk(node, share_of(4, 40), running, waiting, std::ref(taken));

	ASSERT_TRUE(done.whole);

	std::set<std::string> keys = taken_by(taking, taken.taken());

	EXPECT_EQ(40u, keys.size());

	for (size_t i = 0; i < 40; i++)
	{
		EXPECT_EQ(1u, keys.count(key(i))) << key(i);
	}
}

// The other half of what makes a walk fast: the network and the store are busy at the same time
// rather than each waiting for the other. While a file is being taken in, the next is on its way.
TEST(transfer_test, asks_for_the_next_file_while_the_last_one_is_being_taken_in)
{
	repository::fake_repository store;
	cluster::fake_cluster alone(there, std::vector<std::string>());
	router::router router(store, alone);
	serving_node node(router);
	progress::patience waiting(30);

	fill(store, 8);

	std::atomic<size_t> taken = 0;
	std::atomic<bool> overlapped = false;

	// A budget of a few bytes is a file a record, so there are several of them to run ahead of.
	transfer::walk(
		node,
		share_of(1, 4),
		running,
		waiting,
		[&](const std::string &)
		{
			// The file after this one has been asked for, and this one has not been let go of yet.
			if (taken == 0 && asked_at_least(node, 2))
			{
				overlapped = true;
			}

			taken++;
		});

	EXPECT_LT(1u, taken);
	EXPECT_TRUE(overlapped);
}

TEST(transfer_test, a_walk_that_is_no_longer_running_asks_nothing_at_all)
{
	repository::fake_repository store;
	cluster::fake_cluster alone(there, std::vector<std::string>());
	router::router router(store, alone);
	serving_node node(router);
	progress::patience waiting(30);
	collector taken;
	const std::atomic<bool> stopped(false);

	fill(store, 8);

	transfer::outcome done = transfer::walk(node, share_of(4, 4096), stopped, waiting, std::ref(taken));

	EXPECT_FALSE(done.whole);
	EXPECT_FALSE(done.refused);
	EXPECT_EQ(0u, node.asked());
	EXPECT_TRUE(taken.taken().empty());
}

// A node that will not answer is told apart from a walk that ran out of its own patience, because
// the caller does different things about them: one is another zone to ask, the other is not.
TEST(transfer_test, says_when_it_was_the_node_that_refused)
{
	repository::fake_repository store;
	cluster::fake_cluster alone(there, std::vector<std::string>());
	router::router router(store, alone);
	serving_node node(router);
	progress::patience waiting(30);
	collector taken;

	transfer::share wanted = share_of(1, 4096);

	wanted.table = "no_such_table";

	transfer::outcome done = transfer::walk(node, wanted, running, waiting, std::ref(taken));

	EXPECT_FALSE(done.whole);
	EXPECT_TRUE(done.refused);
	EXPECT_TRUE(taken.taken().empty());
}

TEST(transfer_test, a_walk_out_of_patience_is_neither_whole_nor_refused)
{
	repository::fake_repository store;
	cluster::fake_cluster alone(there, std::vector<std::string>());
	router::router router(store, alone);
	serving_node node(router);
	progress::patience waiting(0);
	collector taken;

	fill(store, 8);

	transfer::outcome done = transfer::walk(node, share_of(1, 4096), running, waiting, std::ref(taken));

	EXPECT_FALSE(done.whole);
	EXPECT_FALSE(done.refused);
	EXPECT_EQ(0u, node.asked());
}
