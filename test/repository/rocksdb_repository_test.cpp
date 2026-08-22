#include <memory>
#include <set>
#include <vector>
#include <filesystem>

#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <boost/json/src.hpp>

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

TEST_F(repository_test, write_read_and_delete_a_record)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("a key", "a value"));

	EXPECT_EQ(repository->read_record("a_table", "a key"), "a value");

	repository->delete_record("a_table", "a key");

	EXPECT_EQ(repository->read_record("a_table", "a key"), std::nullopt);
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

TEST_F(repository_test, delete_a_range_of_records)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));
	repository->write_record("a_table", record::valid_record("3", "three"));

	scan::range range = whole_table();

	range.from = "1";
	range.has_from = true;
	range.to = "3";
	range.has_to = true;

	repository->delete_records("a_table", range);

	EXPECT_EQ(keys(repository->scan_records("a_table", whole_table())), (std::vector<std::string> { "3" }));
}

TEST_F(repository_test, delete_a_range_that_runs_to_the_last_key)
{
	create_table("a_table");

	repository->write_record("a_table", record::valid_record("1", "one"));
	repository->write_record("a_table", record::valid_record("2", "two"));
	repository->write_record("a_table", record::valid_record("3", "three"));

	scan::range range = whole_table();

	range.from = "2";
	range.has_from = true;

	repository->delete_records("a_table", range);

	EXPECT_EQ(keys(repository->scan_records("a_table", whole_table())), (std::vector<std::string> { "1" }));
}

TEST_F(repository_test, writes_are_not_stalled)
{
	EXPECT_FALSE(repository->is_write_stalled());
}
