#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>
#include <set>
#include <vector>
#include <filesystem>

#include <gtest/gtest.h>
#include <boost/json/src.hpp>

#include "cluster/partition.h"
#include "repository/rocksdb_repository.h"
#include "table/table.h"

namespace
{
	scan::range whole_table()
	{
		scan::range range;

		range.is_valid = true;

		return range;
	}

	std::vector<std::string> keys(const scan::page &page)
	{
		std::vector<std::string> keys;

		for (size_t i = 0; i < page.records.size(); i++)
		{
			keys.push_back(page.records[i].key);
		}

		return keys;
	}
}

class repository_test : public ::testing::Test
{
protected:
	std::unique_ptr<repository::rocksdb_repository> repository;

	std::unique_ptr<repository::rocksdb_repository> other_repository;

	// The directory is opened in the constructor, so it is emptied before the repository exists
	// rather than in the body of a set up that would already have opened it.
	void SetUp() override
	{
		std::filesystem::remove_all("/tmp/asyncdb/");
		std::filesystem::remove_all("/tmp/asyncdb_other/");

		repository = std::make_unique<repository::rocksdb_repository>("/tmp/asyncdb");
		other_repository = std::make_unique<repository::rocksdb_repository>("/tmp/asyncdb_other");
	}

	void TearDown() override
	{
		repository = nullptr;
		other_repository = nullptr;
	}

	void create_table(const std::string &name)
	{
		repository->create_table(table::valid_table(name, std::vector<std::string>()));
	}
};

TEST_F(repository_test, create_and_read_tables)
{
	create_table("first_table");
	create_table("second_table");

	std::set<table::table> table_set = repository->list_tables();
	std::vector<table::table> tables(table_set.begin(), table_set.end());

	EXPECT_EQ(tables.size(), 2);
	EXPECT_EQ(tables[0].name, "first_table");
	EXPECT_EQ(tables[1].name, "second_table");
}

TEST_F(repository_test, two_instances_do_not_share_a_keyspace)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()));

	repository->write_record("a_table", record::valid_record("a key", "a value"));

	EXPECT_EQ(other_repository->read_record("a_table", "a key"), std::nullopt);

	// A cursor names the iteration it belongs to, and the two instances issue different ones.
	EXPECT_NE(repository->instance(), other_repository->instance());
}

// The point of the store being the directory it was given rather than something random named
// underneath it: an instance that is started again opens what the instance before it wrote, and
// not an empty store beside it.
TEST_F(repository_test, a_store_is_read_back_by_the_instance_started_after_it)
{
	create_table("a_table");
	repository->write_record("a_table", record::valid_record("a key", "a value"));

	// Closed and opened again, which is a container that was restarted or a host that rebooted.
	repository = nullptr;
	repository = std::make_unique<repository::rocksdb_repository>("/tmp/asyncdb");

	EXPECT_TRUE(repository->has_table("a_table"));
	EXPECT_EQ(repository->read_record("a_table", "a key"), "a value");
}

TEST_F(repository_test, read_table)
{
	create_table("a_table");

	table::table table = repository->read_table("a_table");

	EXPECT_TRUE(table.is_valid);
	EXPECT_EQ(table.name, "a_table");
	EXPECT_EQ(table.json["name"].as_string(), "a_table");
	EXPECT_EQ(table.json["dependencies"].as_array().size(), 0);
}

TEST_F(repository_test, fail_to_read_table_that_does_not_exist)
{
	create_table("a_table");

	table::table table = repository->read_table("not_a_table");

	EXPECT_FALSE(table.is_valid);
	EXPECT_EQ(table.name, "");
	EXPECT_EQ(table.code, "table_not_found");
}

TEST_F(repository_test, does_not_have_invalid_table)
{
	repository->create_table(table::invalid_table("invalid_table_name", "error"));

	EXPECT_EQ(repository->list_tables().size(), 0);
}

TEST_F(repository_test, has_valid_table)
{
	create_table("a_table");

	EXPECT_TRUE(repository->has_table("a_table"));
}

TEST_F(repository_test, does_not_have_table)
{
	create_table("a_table");

	EXPECT_FALSE(repository->has_table("not_a_table"));
}

TEST_F(repository_test, does_not_delete_a_table_that_is_not_there)
{
	create_table("a_table");

	repository->delete_table("not_a_table");

	EXPECT_TRUE(repository->has_table("a_table"));
}

TEST_F(repository_test, delete_table_from_repository)
{
	create_table("a_table");

	repository->delete_table("a_table");

	EXPECT_FALSE(repository->has_table("a_table"));
	EXPECT_EQ(repository->list_tables().size(), 0);
}

TEST_F(repository_test, the_data_goes_with_the_table)
{
	create_table("a_table");
	repository->write_record("a_table", record::valid_record("a key", "a value"));

	repository->delete_table("a_table");
	create_table("a_table");

	EXPECT_EQ(repository->read_record("a_table", "a key"), std::nullopt);
}

TEST_F(repository_test, two_tables_may_hold_the_same_key)
{
	create_table("first_table");
	create_table("second_table");

	repository->write_record("first_table", record::valid_record("a key", "first value"));
	repository->write_record("second_table", record::valid_record("a key", "second value"));

	EXPECT_EQ(repository->read_record("first_table", "a key"), "first value");
	EXPECT_EQ(repository->read_record("second_table", "a key"), "second value");
}

TEST_F(repository_test, write_and_read_a_record)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("a key", "a value"));

	EXPECT_EQ(repository->read_record("a_table", "a key"), "a value");
}

TEST_F(repository_test, an_empty_value_is_a_value)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("a key", ""));

	EXPECT_EQ(repository->read_record("a_table", "a key"), "");
}

TEST_F(repository_test, fail_to_write_a_record_to_a_table_that_is_not_there)
{
	EXPECT_THROW(
		repository->write_record("not_a_table", record::valid_record("a key", "a value")),
		repository::storage_error);
}

TEST_F(repository_test, scan_records_in_key_order)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("user:7203", "Marcus Hale"));
	repository->write_record("a_table", record::valid_record("user:4821", "Eleanor Whitmore"));
	repository->write_record("a_table", record::valid_record("order:1", "an order"));

	scan::page page = repository->scan_records("a_table", whole_table());

	EXPECT_EQ(keys(page), (std::vector<std::string> { "order:1", "user:4821", "user:7203" }));
	EXPECT_EQ(page.records[1].value, "Eleanor Whitmore");
	EXPECT_FALSE(page.has_more);
}

TEST_F(repository_test, scan_a_range)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));
	repository->write_record("a_table", record::valid_record("3", "three"));

	scan::range range = whole_table();

	range.from = "2";
	range.has_from = true;
	range.to = "3";
	range.has_to = true;

	EXPECT_EQ(keys(repository->scan_records("a_table", range)), (std::vector<std::string> { "2" }));
}

TEST_F(repository_test, scan_backwards)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));
	repository->write_record("a_table", record::valid_record("3", "three"));

	scan::range range = whole_table();

	range.reverse = true;
	range.from = "1";
	range.has_from = true;
	range.to = "3";
	range.has_to = true;

	EXPECT_EQ(keys(repository->scan_records("a_table", range)), (std::vector<std::string> { "2", "1" }));
}

TEST_F(repository_test, a_page_says_whether_there_is_another)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));
	repository->write_record("a_table", record::valid_record("3", "three"));

	scan::range range = whole_table();

	range.limit = 2;

	scan::page page = repository->scan_records("a_table", range);

	EXPECT_EQ(keys(page), (std::vector<std::string> { "1", "2" }));
	EXPECT_TRUE(page.has_more);

	range.limit = 3;

	EXPECT_FALSE(repository->scan_records("a_table", range).has_more);
}

// A limit of a thousand says nothing about the size of a thousand records, and a value may be
// 16 MiB. Six of two megabytes is over the eight megabyte budget, so the page ends at three and
// says there is more — which is the cursor the client follows, and not a response nobody can
// hold. Three and not four because the keys are weighed too: what the budget bounds is the
// response, and a key is up to 4 KiB of one.
TEST_F(repository_test, scan_stops_at_the_page_budget_rather_than_the_limit)
{
	create_table("a_table");

	std::string value(2 * 1024 * 1024, 'v');

	for (size_t i = 0; i < 6; i++)
	{
		repository->write_record("a_table", record::valid_record(std::to_string(i), value));
	}

	scan::page page = repository->scan_records("a_table", whole_table());

	EXPECT_EQ(page.records.size(), 3u);
	EXPECT_TRUE(page.has_more);
}

// The budget is never weighed against an empty page, because a scan that could not carry the
// record in front of it would never get past that key.
TEST_F(repository_test, a_record_larger_than_the_budget_is_a_page_of_its_own)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("1", std::string(scan::max_page_bytes + 1, 'v')));
	repository->write_record("a_table", record::valid_record("2", "small"));

	scan::page page = repository->scan_records("a_table", whole_table());

	EXPECT_EQ(keys(page), (std::vector<std::string> { "1" }));
	EXPECT_TRUE(page.has_more);
}

// Keys only is the whole saving on a table of large values: the values are never read, so they
// are not what the page is weighed against either.
TEST_F(repository_test, the_budget_weighs_the_values_a_scan_is_asked_for)
{
	create_table("a_table");

	std::string value(2 * 1024 * 1024, 'v');

	for (size_t i = 0; i < 6; i++)
	{
		repository->write_record("a_table", record::valid_record(std::to_string(i), value));
	}

	scan::range range = whole_table();

	range.values = false;

	scan::page page = repository->scan_records("a_table", range);

	EXPECT_EQ(page.records.size(), 6u);
	EXPECT_FALSE(page.has_more);
}

TEST_F(repository_test, scan_keys_without_their_values)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("a key", "a value"));

	scan::range range = whole_table();

	range.values = false;

	scan::page page = repository->scan_records("a_table", range);

	EXPECT_EQ(page.records[0].key, "a key");
	EXPECT_EQ(page.records[0].value, "");
}

TEST_F(repository_test, writes_are_not_stalled)
{
	EXPECT_FALSE(repository->is_write_stalled());
}

namespace
{
	repository::share every_partition()
	{
		repository::share wanted;

		wanted.partitions.set();

		return wanted;
	}

	record::record versioned(const std::string &key, const std::string &value, uint64_t term, uint64_t count)
	{
		record::record written = record::valid_record(key, value);

		written.stamp = record::version { term, count };

		return written;
	}
}

// The whole of what a rebuild does, over the two stores rather than over the network: a share is
// one file, and the node it reaches reads it back as records of its own.
TEST_F(repository_test, a_file_carries_a_table_from_one_store_to_another)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()));

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));

	repository::extract taken = repository->export_records("a_table", every_partition());

	EXPECT_EQ(2u, taken.records);
	EXPECT_FALSE(taken.has_more);
	EXPECT_EQ(2u, other_repository->import_records("a_table", taken.file));

	EXPECT_EQ("one", other_repository->read_record("a_table", "1").value_or(""));
	EXPECT_EQ("two", other_repository->read_record("a_table", "2").value_or(""));
}

TEST_F(repository_test, a_file_carries_the_partitions_it_was_asked_for_and_no_others)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()));

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));

	repository::share wanted;

	wanted.partitions.set(cluster::partition_of("1"));

	repository::extract taken = repository->export_records("a_table", wanted);

	EXPECT_EQ(1u, taken.records);
	EXPECT_EQ(1u, other_repository->import_records("a_table", taken.file));

	EXPECT_TRUE(other_repository->read_record("a_table", "1").has_value());
	EXPECT_FALSE(other_repository->read_record("a_table", "2").has_value());
}

TEST_F(repository_test, a_file_that_carried_nothing_is_no_file_at_all)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()));

	repository->write_record("a_table", record::valid_record("1", "one"));

	repository::extract taken = repository->export_records("a_table", repository::share());

	EXPECT_EQ(0u, taken.records);
	EXPECT_TRUE(taken.file.empty());
	EXPECT_EQ(0u, other_repository->import_records("a_table", taken.file));
}

TEST_F(repository_test, an_export_of_a_table_holding_nothing_carries_nothing)
{
	create_table("a_table");

	repository::extract taken = repository->export_records("a_table", every_partition());

	EXPECT_EQ(0u, taken.records);
	EXPECT_FALSE(taken.has_more);
	EXPECT_TRUE(taken.file.empty());
}

// Two copies of one write carry one version, which is a pair neither side can order — and what a
// store does about a record it cannot order is keep the one it has.
TEST_F(repository_test, a_store_keeps_what_it_holds_already_when_a_file_carries_that_key_too)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()));

	repository->write_record("a_table", record::valid_record("1", "theirs"));
	repository->write_record("a_table", record::valid_record("2", "theirs"));

	other_repository->write_record("a_table", record::valid_record("1", "mine"));

	repository::extract taken = repository->export_records("a_table", every_partition());

	EXPECT_EQ(1u, other_repository->import_records("a_table", taken.file));

	EXPECT_EQ("mine", other_repository->read_record("a_table", "1").value_or(""));
	EXPECT_EQ("theirs", other_repository->read_record("a_table", "2").value_or(""));
}

// What catches a copy up. The record in the file was written after the one held, so the node
// holding the older of the two was not there for that write, and this is the pass that gives it
// to it.
TEST_F(repository_test, a_store_takes_a_record_from_a_file_written_after_the_one_it_holds)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()));

	repository->write_record("a_table", versioned("1", "theirs", 41, 9));
	other_repository->write_record("a_table", versioned("1", "mine", 41, 4));

	repository::extract taken = repository->export_records("a_table", every_partition());

	EXPECT_EQ(1u, other_repository->import_records("a_table", taken.file));
	EXPECT_EQ("theirs", other_repository->read_record("a_table", "1").value_or(""));
}

// And the other way round, which is what makes a fetch safe: a record written after the one a file
// carries is one the file cannot undo, whoever owns the key now.
TEST_F(repository_test, a_store_keeps_a_record_written_after_the_one_a_file_carries)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()));

	repository->write_record("a_table", versioned("1", "theirs", 41, 4));
	other_repository->write_record("a_table", versioned("1", "mine", 41, 9));

	repository::extract taken = repository->export_records("a_table", every_partition());

	EXPECT_EQ(0u, other_repository->import_records("a_table", taken.file));
	EXPECT_EQ("mine", other_repository->read_record("a_table", "1").value_or(""));
}

// A store holding none of the keys a file carries takes the file as it stands, which is the path a
// rebuild runs down. What that skips is the comparison, so the version has to travel in the file
// itself rather than be applied as it is taken in.
TEST_F(repository_test, a_file_taken_whole_carries_the_versions_it_was_written_with)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()));

	repository->write_record("a_table", versioned("1", "one", 41, 9));

	repository::extract taken = repository->export_records("a_table", every_partition());

	EXPECT_EQ(1u, other_repository->import_records("a_table", taken.file));

	// Written before what the store now holds, so it is refused — which it would not be if the
	// file had been taken in with a version of its own.
	other_repository->write_record("a_table", versioned("1", "older", 41, 8));

	EXPECT_EQ("older", other_repository->read_record("a_table", "1").value_or(""));

	repository->write_record("a_table", versioned("1", "newer", 41, 10));

	repository::extract again = repository->export_records("a_table", every_partition());

	EXPECT_EQ(1u, other_repository->import_records("a_table", again.file));
	EXPECT_EQ("newer", other_repository->read_record("a_table", "1").value_or(""));
}

// The keys alone are what a node asks for when it is deciding whether to give a record up, and the
// version is what it has to decide on — so a file carrying no values carries the versions.
TEST_F(repository_test, a_store_keeps_a_record_the_node_that_owns_it_has_yet_to_catch_up_on)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()));

	// The owner's copy, and the one being given up, which was written after it.
	repository->write_record("a_table", versioned("1", "the owner\'s", 41, 4));
	repository->write_record("a_table", versioned("2", "the owner\'s", 41, 9));

	other_repository->write_record("a_table", versioned("1", "later", 41, 9));
	other_repository->write_record("a_table", versioned("2", "earlier", 41, 4));

	repository::share wanted = every_partition();

	wanted.values = false;

	repository::extract taken = repository->export_records("a_table", wanted);

	EXPECT_EQ(1u, other_repository->clear_records("a_table", taken.file));
	EXPECT_EQ("later", other_repository->read_record("a_table", "1").value_or(""));
	EXPECT_FALSE(other_repository->read_record("a_table", "2").has_value());
}

// A count outlives the process that issued it, because a leader that keeps its claim across a
// restart goes on stamping writes in the same term: a count that started again would be a version
// this store had already used.
TEST_F(repository_test, counts_rise_across_the_store_being_opened_again)
{
	uint64_t last = repository->next_count();

	EXPECT_GT(repository->next_count(), last);

	last = repository->next_count();
	repository = nullptr;
	repository = std::make_unique<repository::rocksdb_repository>("/tmp/asyncdb");

	EXPECT_GT(repository->next_count(), last);
}

// A store written before records carried a version holds values that are values all the way
// through, so opening one and reading the first bytes of each as a version would serve bytes
// nobody wrote. There is nothing here that can turn one into the other, and refusing to open it
// is what keeps it from being served.
TEST_F(repository_test, a_store_written_before_records_carried_a_version_is_refused)
{
	create_table("a_table");
	repository->write_record("a_table", record::valid_record("1", "one"));
	repository = nullptr;

	// Which is a store with a table in it and nothing saying what its values are.
	{
		rocksdb::Options options;
		std::vector<std::string> names;

		ASSERT_TRUE(rocksdb::DB::ListColumnFamilies(options, "/tmp/asyncdb", &names).ok());

		std::vector<rocksdb::ColumnFamilyDescriptor> families;

		for (size_t i = 0; i < names.size(); i++)
		{
			families.push_back(rocksdb::ColumnFamilyDescriptor(names[i], rocksdb::ColumnFamilyOptions()));
		}

		std::vector<rocksdb::ColumnFamilyHandle *> handles;
		std::unique_ptr<rocksdb::DB> database;

		ASSERT_TRUE(rocksdb::DB::Open(options, "/tmp/asyncdb", families, &handles, &database).ok());
		ASSERT_TRUE(database->Delete(rocksdb::WriteOptions(), "FORMAT").ok());

		for (size_t i = 0; i < handles.size(); i++)
		{
			database->DestroyColumnFamilyHandle(handles[i]);
		}
	}

	EXPECT_THROW(repository::rocksdb_repository("/tmp/asyncdb"), repository::storage_error);
}

// A share larger than one file is several of them, resumed from the key the walk reached rather
// than from the last key written: a file the partitions emptied still moved the walk along.
TEST_F(repository_test, a_walk_larger_than_one_file_resumes_where_it_reached)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()));

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));
	repository->write_record("a_table", record::valid_record("3", "three"));

	repository::share wanted = every_partition();

	// One record at a time, which is a budget the first record of any file spends.
	wanted.bytes = 1;

	repository::extract first = repository->export_records("a_table", wanted);

	ASSERT_TRUE(first.has_more);
	EXPECT_EQ("1", first.last);
	EXPECT_EQ(1u, other_repository->import_records("a_table", first.file));

	wanted.from = first.last;
	wanted.has_from = true;

	repository::extract second = repository->export_records("a_table", wanted);

	ASSERT_TRUE(second.has_more);
	EXPECT_EQ("2", second.last);
	EXPECT_EQ(1u, other_repository->import_records("a_table", second.file));

	wanted.from = second.last;

	repository::extract third = repository->export_records("a_table", wanted);

	EXPECT_FALSE(third.has_more);
	EXPECT_EQ(1u, other_repository->import_records("a_table", third.file));

	EXPECT_EQ(
		keys(other_repository->scan_records("a_table", whole_table())),
		(std::vector<std::string> { "1", "2", "3" }));
}

// The budget an operator sizes to the instance. It is the block cache and the memtables together,
// and a store given a small one is a store that still opens and still answers.
TEST_F(repository_test, a_store_serves_within_the_memory_budget_it_was_given)
{
	std::filesystem::remove_all("/tmp/asyncdb_small/");

	repository::rocksdb_repository small("/tmp/asyncdb_small", 16 * 1024 * 1024);

	small.create_table(table::valid_table("a_table", std::vector<std::string>()));
	small.write_record("a_table", record::valid_record("1", "one"));

	EXPECT_EQ("one", small.read_record("a_table", "1").value_or(""));
}

// A transfer that was in flight when the process before this one went leaves a file behind, and
// there is nothing to resume it with. The store opening over it is what clears it away, and the
// store itself opens as it was.
TEST_F(repository_test, a_transfer_the_process_before_it_left_behind_goes_when_the_store_opens)
{
	create_table("a_table");
	repository->write_record("a_table", record::valid_record("a key", "a value"));

	std::filesystem::path left("/tmp/asyncdb/transfer/left-behind.sst");

	std::ofstream(left) << "half of a file nothing can finish";

	ASSERT_TRUE(std::filesystem::exists(left));

	repository = nullptr;
	repository = std::make_unique<repository::rocksdb_repository>("/tmp/asyncdb");

	EXPECT_FALSE(std::filesystem::exists(left));
	EXPECT_EQ(repository->read_record("a_table", "a key"), "a value");
}

// What a clear down asks the node that owns a share: which of these keys have you got. The answer
// is a file, so it is one question for a share rather than one for every record in it.
TEST_F(repository_test, a_file_of_keys_is_what_a_store_gives_records_up_on)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()));

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));

	// The node giving records up holds one the owner has and one it has not.
	other_repository->write_record("a_table", record::valid_record("1", "mine"));
	other_repository->write_record("a_table", record::valid_record("3", "mine"));

	repository::share wanted = every_partition();

	wanted.values = false;

	repository::extract taken = repository->export_records("a_table", wanted);

	EXPECT_EQ(2u, taken.records);
	EXPECT_EQ(1u, other_repository->clear_records("a_table", taken.file));

	// The one the owner has is gone, and the one it has never held is kept — and a key that was
	// never here is not a tombstone either.
	EXPECT_FALSE(other_repository->read_record("a_table", "1").has_value());
	EXPECT_TRUE(other_repository->read_record("a_table", "3").has_value());
	EXPECT_EQ(keys(other_repository->scan_records("a_table", whole_table())), (std::vector<std::string> { "3" }));
}

// The budget is what the walk read, so a walk that is not reading values covers far more of a table
// for the same one. That is what makes clearing down a share one question and not thousands.
TEST_F(repository_test, a_walk_of_keys_alone_covers_more_of_a_table_than_a_walk_of_records)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("1", std::string(4096, 'x')));
	repository->write_record("a_table", record::valid_record("2", std::string(4096, 'x')));
	repository->write_record("a_table", record::valid_record("3", std::string(4096, 'x')));

	repository::share wanted = every_partition();

	// A budget a single value spends and a great many keys do not.
	wanted.bytes = 2048;

	repository::extract records = repository->export_records("a_table", wanted);

	wanted.values = false;

	repository::extract just_keys = repository->export_records("a_table", wanted);

	EXPECT_EQ(1u, records.records);
	EXPECT_TRUE(records.has_more);

	EXPECT_EQ(3u, just_keys.records);
	EXPECT_FALSE(just_keys.has_more);
}

// Where a walk of this table would be cut up, so that several workers can read it at once. It is
// weighed by what is in the files a key starts, so it is approximate — which is all it has to be.
TEST_F(repository_test, says_where_a_table_would_be_cut_up)
{
	std::filesystem::remove_all("/tmp/asyncdb_split/");

	// The smallest budget there is, so that what is written spills out of the memtable and into the
	// files a split is read off rather than sitting in memory where it cannot be seen.
	repository::rocksdb_repository splitting("/tmp/asyncdb_split", 16 * 1024 * 1024);

	splitting.create_table(table::valid_table("a_table", std::vector<std::string>()));

	for (size_t i = 0; i < 320; i++)
	{
		splitting.write_record("a_table", record::valid_record(std::to_string(10000 + i), std::string(32 * 1024, 'x')));
	}

	// A memtable full is a flush scheduled and not a flush done, so the answer is waited for rather
	// than read once.
	std::vector<std::string> points = splitting.split_points("a_table", 4);

	for (size_t i = 0; i < 100 && points.empty(); i++)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));

		points = splitting.split_points("a_table", 4);
	}

	ASSERT_FALSE(points.empty());
	EXPECT_GE(3u, points.size());
	EXPECT_TRUE(std::is_sorted(points.begin(), points.end()));

	// Every one of them is a key of the table, which is what makes it a bound records fall either
	// side of.
	for (size_t i = 0; i < points.size(); i++)
	{
		EXPECT_TRUE(splitting.read_record("a_table", points[i]).has_value()) << points[i];
	}
}

TEST_F(repository_test, a_table_that_is_not_worth_cutting_up_is_cut_up_no_ways)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("1", "one"));

	EXPECT_TRUE(repository->split_points("a_table", 1).empty());
	EXPECT_TRUE(repository->split_points("a_table", 0).empty());
}

// The pieces of a split walk have to be a cover, and that is what the ends of a share mean: `from`
// is the key the piece starts after and `to` is the last key in it, so the key one piece stops at
// is the key the next one starts after.
TEST_F(repository_test, the_pieces_of_a_share_are_every_record_and_no_record_twice)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()));

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));
	repository->write_record("a_table", record::valid_record("3", "three"));
	repository->write_record("a_table", record::valid_record("4", "four"));

	repository::share first = every_partition();
	repository::share second = every_partition();

	first.to = "2";
	first.has_to = true;

	second.from = "2";
	second.has_from = true;

	EXPECT_EQ(2u, other_repository->import_records("a_table", repository->export_records("a_table", first).file));
	EXPECT_EQ(2u, other_repository->import_records("a_table", repository->export_records("a_table", second).file));

	EXPECT_EQ(
		keys(other_repository->scan_records("a_table", whole_table())),
		(std::vector<std::string> { "1", "2", "3", "4" }));
}

TEST_F(repository_test, a_piece_of_a_share_ends_where_it_was_told_to)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));
	repository->write_record("a_table", record::valid_record("3", "three"));

	repository::share wanted = every_partition();

	wanted.to = "2";
	wanted.has_to = true;

	repository::extract taken = repository->export_records("a_table", wanted);

	EXPECT_EQ(2u, taken.records);
	EXPECT_FALSE(taken.has_more);
}
