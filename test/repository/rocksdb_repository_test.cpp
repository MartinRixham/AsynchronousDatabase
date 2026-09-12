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
#include "table/schema.h"

namespace
{
	// A scan reads one partition, so a range is of the partition a key is in. The records of one
	// partition key are one of them, which is what these scans are written against.
	scan::range one_partition(const std::string &key)
	{
		scan::range range;

		range.is_valid = true;
		range.partition = cluster::partition_of(key);

		return range;
	}

	// The sort halves of a page, which is what the records of one partition key differ by.
	std::vector<std::string> sorts(const scan::page &page)
	{
		std::vector<std::string> sorts;

		for (size_t i = 0; i < page.records.size(); i++)
		{
			sorts.push_back(std::string(record::sort_key(page.records[i].key)));
		}

		return sorts;
	}

	// Every key a store holds of a table, which is a scan of each partition in turn: the store
	// sorts the partitions apart, so there is no answer that is a whole table.
	std::vector<std::string> every_key(const repository::repository &store, const std::string &name)
	{
		std::vector<std::string> found;

		for (size_t partition = 0; partition < cluster::partition_count; partition++)
		{
			scan::range range;

			range.is_valid = true;
			range.partition = partition;

			scan::page page = store.scan_records(name, range);

			for (size_t i = 0; i < page.records.size(); i++)
			{
				found.push_back(page.records[i].key);
			}
		}

		std::sort(found.begin(), found.end());

		return found;
	}

	record::record under(const std::string &key, const std::string &sort, const std::string &value)
	{
		return record::valid_record(record::compose_key(key, sort), value);
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
		repository->create_table(table::valid_table(name, std::vector<std::string>()), record::version { 1, 1 });
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
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

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

// The schema is one record, so what survives a restart is the whole of it — the names the store
// has dropped as well as the ones it holds. A tombstone that did not survive would be a node that
// takes a dropped table back from a peer the first time it reconciles.
TEST_F(repository_test, the_names_a_store_dropped_are_read_back_with_the_ones_it_holds)
{
	create_table("a_table");
	create_table("dropped_table");

	repository->delete_table("dropped_table", record::version { 1, 2 });

	repository = nullptr;
	repository = std::make_unique<repository::rocksdb_repository>("/tmp/asyncdb");

	table::schema read = repository->read_schema();

	EXPECT_TRUE(read.has("a_table"));
	ASSERT_TRUE(read.read_entry("dropped_table").has_value());
	EXPECT_FALSE(read.read_entry("dropped_table")->live);
	EXPECT_EQ(read.read_entry("dropped_table")->stamp.count, 2u);
}

// A merge is the pass reaching the store: the column family of a name that arrived is made, and
// the one a tombstone took away is dropped with the records in it.
TEST_F(repository_test, a_merged_schema_makes_and_drops_the_column_families_to_match)
{
	create_table("going");
	repository->write_record("going", record::valid_record("a key", "a value"));

	table::schema named;

	named.create(table::valid_table("arriving", std::vector<std::string>()), record::version { 1, 2 });
	named.remove("going", record::version { 1, 2 });

	EXPECT_EQ(repository->merge_schema(named), 2u);
	EXPECT_TRUE(repository->has_table("arriving"));
	EXPECT_FALSE(repository->has_table("going"));

	// Writable, which is what says the family was made rather than only the name.
	repository->write_record("arriving", record::valid_record("a key", "a value"));

	EXPECT_EQ(repository->read_record("arriving", "a key"), "a value");

	// And the records went with the family, so the name coming back is an empty table.
	repository->create_table(table::valid_table("going", std::vector<std::string>()), record::version { 1, 3 });

	EXPECT_EQ(repository->read_record("going", "a key"), std::nullopt);
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
	repository->create_table(table::invalid_table("invalid_table_name", "error"), record::version { 1, 1 });

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

	repository->delete_table("not_a_table", record::version { 1, 2 });

	EXPECT_TRUE(repository->has_table("a_table"));
}

TEST_F(repository_test, delete_table_from_repository)
{
	create_table("a_table");

	repository->delete_table("a_table", record::version { 1, 2 });

	EXPECT_FALSE(repository->has_table("a_table"));
	EXPECT_EQ(repository->list_tables().size(), 0);
}

TEST_F(repository_test, the_data_goes_with_the_table)
{
	create_table("a_table");
	repository->write_record("a_table", record::valid_record("a key", "a value"));

	repository->delete_table("a_table", record::version { 1, 2 });
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

// **A scan reads one partition**, because the store holds a record under its partition: the keys
// of a table are spread over all of them, and what one scan answers with is the records that
// hashed to the partition it named.
TEST_F(repository_test, scan_a_partition_in_key_order)
{
	create_table("a_table");

	repository->write_record("a_table", under("user", "7203", "Marcus Hale"));
	repository->write_record("a_table", under("user", "4821", "Eleanor Whitmore"));
	repository->write_record("a_table", record::valid_record("order:1", "an order"));

	scan::page page = repository->scan_records("a_table", one_partition("user"));

	EXPECT_EQ(sorts(page), (std::vector<std::string> { "4821", "7203" }));
	EXPECT_EQ(page.records[0].value, "Eleanor Whitmore");
	EXPECT_FALSE(page.has_more);
}

// A key of another partition is not in the range whatever the bounds say, because the store sorts
// the partitions apart: it is not a key this scan passes over but one it never reaches.
TEST_F(repository_test, a_scan_of_one_partition_reads_nothing_of_another)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("order:1", "an order"));

	EXPECT_TRUE(repository->scan_records("a_table", one_partition("user")).records.empty());
	EXPECT_EQ(keys(repository->scan_records("a_table", one_partition("order:1"))),
		(std::vector<std::string> { "order:1" }));
}

TEST_F(repository_test, scan_a_range)
{
	create_table("a_table");

	repository->write_record("a_table", under("n", "1", "one"));
	repository->write_record("a_table", under("n", "2", "two"));
	repository->write_record("a_table", under("n", "3", "three"));

	scan::range range = one_partition("n");

	range.from = record::compose_key("n", "2");
	range.has_from = true;
	range.to = record::compose_key("n", "3");
	range.has_to = true;

	EXPECT_EQ(sorts(repository->scan_records("a_table", range)), (std::vector<std::string> { "2" }));
}

TEST_F(repository_test, scan_backwards)
{
	create_table("a_table");

	repository->write_record("a_table", under("n", "1", "one"));
	repository->write_record("a_table", under("n", "2", "two"));
	repository->write_record("a_table", under("n", "3", "three"));

	scan::range range = one_partition("n");

	range.reverse = true;
	range.from = record::compose_key("n", "1");
	range.has_from = true;
	range.to = record::compose_key("n", "3");
	range.has_to = true;

	EXPECT_EQ(sorts(repository->scan_records("a_table", range)), (std::vector<std::string> { "2", "1" }));
}

TEST_F(repository_test, a_page_says_whether_there_is_another)
{
	create_table("a_table");

	repository->write_record("a_table", under("n", "1", "one"));
	repository->write_record("a_table", under("n", "2", "two"));
	repository->write_record("a_table", under("n", "3", "three"));

	scan::range range = one_partition("n");

	range.limit = 2;

	scan::page page = repository->scan_records("a_table", range);

	EXPECT_EQ(sorts(page), (std::vector<std::string> { "1", "2" }));
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
		repository->write_record("a_table", under("n", std::to_string(i), value));
	}

	scan::page page = repository->scan_records("a_table", one_partition("n"));

	EXPECT_EQ(page.records.size(), 3u);
	EXPECT_TRUE(page.has_more);
}

// The budget is never weighed against an empty page, because a scan that could not carry the
// record in front of it would never get past that key.
TEST_F(repository_test, a_record_larger_than_the_budget_is_a_page_of_its_own)
{
	create_table("a_table");

	repository->write_record("a_table", under("n", "1", std::string(scan::max_page_bytes + 1, 'v')));
	repository->write_record("a_table", under("n", "2", "small"));

	scan::page page = repository->scan_records("a_table", one_partition("n"));

	EXPECT_EQ(sorts(page), (std::vector<std::string> { "1" }));
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
		repository->write_record("a_table", under("n", std::to_string(i), value));
	}

	scan::range range = one_partition("n");

	range.values = false;

	scan::page page = repository->scan_records("a_table", range);

	EXPECT_EQ(page.records.size(), 6u);
	EXPECT_FALSE(page.has_more);
}

TEST_F(repository_test, scan_keys_without_their_values)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("a key", "a value"));

	scan::range range = one_partition("a key");

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

	// A partition this table holds no record in, which is what a share of one is asked for below.
	size_t empty_partition(const repository::repository &store, const std::string &name)
	{
		for (size_t partition = 0; partition < cluster::partition_count; partition++)
		{
			scan::range range;

			range.is_valid = true;
			range.partition = partition;
			range.values = false;

			if (store.scan_records(name, range).records.empty())
			{
				return partition;
			}
		}

		return 0;
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
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

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
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

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

// **What a walk reads is what it carries.** The budget is spent on the records of the share and
// never on the table in between: a partition holding one record answers with it however much of
// the table belongs to the other nodes of the zone, where a walk that read its way through the
// table would have spent the budget before it reached the key.
TEST_F(repository_test, a_share_of_one_partition_is_not_bounded_by_what_the_rest_of_the_table_holds)
{
	create_table("a_table");

	std::string value(4 * 1024, 'v');

	for (size_t i = 0; i < 200; i++)
	{
		repository->write_record("a_table", record::valid_record("key:" + std::to_string(i), value));
	}

	repository::share wanted;

	wanted.partitions.set(cluster::partition_of("key:137"));

	// A budget one record spends, so a walk that read anything it does not carry would carry
	// nothing at all.
	wanted.bytes = 1;

	repository::extract taken = repository->export_records("a_table", wanted);

	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

	EXPECT_EQ(1u, taken.records);
	EXPECT_EQ(1u, other_repository->import_records("a_table", taken.file));
	EXPECT_TRUE(other_repository->read_record("a_table", "key:137").has_value());
}

// And a share the table holds nothing of is one file rather than a walk of the table: the store
// seeks to each partition of the share and finds it empty, instead of reading every key to find
// out that none of them belonged.
TEST_F(repository_test, a_share_of_partitions_a_table_holds_nothing_in_is_read_in_one_file)
{
	create_table("a_table");

	std::string value(4 * 1024, 'v');

	for (size_t i = 0; i < 200; i++)
	{
		repository->write_record("a_table", record::valid_record("key:" + std::to_string(i), value));
	}

	repository::share wanted;

	// One partition of the two hundred and fifty six, and the keys above are not in it.
	wanted.partitions.set(empty_partition(*repository, "a_table"));
	wanted.bytes = 1;

	repository::extract taken = repository->export_records("a_table", wanted);

	EXPECT_EQ(0u, taken.records);
	EXPECT_FALSE(taken.has_more);
	EXPECT_TRUE(taken.file.empty());
}

TEST_F(repository_test, a_file_that_carried_nothing_is_no_file_at_all)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

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
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

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
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

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
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

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
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

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
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

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

// A share larger than one file is several of them, resumed from the key the walk reached — which
// is a key of the store and not one a client would name, the partition being in front of it. A
// budget spent on the last record of a share cannot know it was the last, so the walk asks once
// more and is answered a file with nothing in it.
TEST_F(repository_test, a_walk_larger_than_one_file_resumes_where_it_reached)
{
	create_table("a_table");
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));
	repository->write_record("a_table", record::valid_record("3", "three"));

	repository::share wanted = every_partition();

	// One record at a time, which is a budget the first record of any file spends.
	wanted.bytes = 1;

	size_t files = 0;
	size_t taken = 0;

	for (bool going = true; going; files++)
	{
		ASSERT_LT(files, 10u);

		repository::extract file = repository->export_records("a_table", wanted);

		taken += other_repository->import_records("a_table", file.file);
		going = file.has_more;

		wanted.from = file.last;
		wanted.has_from = true;
	}

	EXPECT_EQ(4u, files);
	EXPECT_EQ(3u, taken);
	EXPECT_EQ(every_key(*other_repository, "a_table"), (std::vector<std::string> { "1", "2", "3" }));
}

// The budget an operator sizes to the instance. It is the block cache and the memtables together,
// and a store given a small one is a store that still opens and still answers.
TEST_F(repository_test, a_store_serves_within_the_memory_budget_it_was_given)
{
	std::filesystem::remove_all("/tmp/asyncdb_small/");

	repository::rocksdb_repository small("/tmp/asyncdb_small", 16 * 1024 * 1024);

	small.create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });
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
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

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
	EXPECT_EQ(every_key(*other_repository, "a_table"), (std::vector<std::string> { "3" }));
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

	splitting.create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

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

	// Every one of them is a bound the records fall either side of, which is the whole of what a
	// split point is: the pieces they cut the table into are every record and no record twice. The
	// keys are the store's own, so what they are is never asked here — only what they bound.
	size_t carried = 0;

	for (size_t i = 0; i <= points.size(); i++)
	{
		repository::share piece = every_partition();

		if (i > 0)
		{
			piece.from = points[i - 1];
			piece.has_from = true;
		}

		if (i < points.size())
		{
			piece.to = points[i];
			piece.has_to = true;
		}

		carried += splitting.export_records("a_table", piece).records;
	}

	EXPECT_EQ(320u, carried);
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
	other_repository->create_table(table::valid_table("a_table", std::vector<std::string>()), record::version { 1, 1 });

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));
	repository->write_record("a_table", record::valid_record("3", "three"));
	repository->write_record("a_table", record::valid_record("4", "four"));

	repository::share first = every_partition();
	repository::share second = every_partition();

	// The bound is a key the store named rather than one made up here: a walk of one record at a
	// time reaches the second of them, and that is where the two pieces meet.
	repository::share stepping = every_partition();

	stepping.bytes = 1;

	stepping.from = repository->export_records("a_table", stepping).last;
	stepping.has_from = true;

	std::string middle = repository->export_records("a_table", stepping).last;

	first.to = middle;
	first.has_to = true;

	second.from = middle;
	second.has_from = true;

	EXPECT_EQ(2u, other_repository->import_records("a_table", repository->export_records("a_table", first).file));
	EXPECT_EQ(2u, other_repository->import_records("a_table", repository->export_records("a_table", second).file));

	EXPECT_EQ(every_key(*other_repository, "a_table"), (std::vector<std::string> { "1", "2", "3", "4" }));
}

TEST_F(repository_test, a_piece_of_a_share_ends_where_it_was_told_to)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));
	repository->write_record("a_table", record::valid_record("3", "three"));

	repository::share stepping = every_partition();

	stepping.bytes = 1;

	repository::share wanted = every_partition();

	// The first key the store holds of this share, which is where the piece is told to end.
	wanted.to = repository->export_records("a_table", stepping).last;
	wanted.has_to = true;

	repository::extract taken = repository->export_records("a_table", wanted);

	EXPECT_EQ(1u, taken.records);
	EXPECT_FALSE(taken.has_more);
}
