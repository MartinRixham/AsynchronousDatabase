#include <gtest/gtest.h>

#include "table/schema.h"

namespace
{
	table::table named(const std::string &name)
	{
		return table::valid_table(name, std::vector<std::string>());
	}

	table::schema holding(const std::string &name, const record::version &stamp)
	{
		table::schema schema;

		schema.create(named(name), stamp);

		return schema;
	}
}

TEST(schema_test, a_created_table_is_there)
{
	table::schema schema = holding("account", record::version { 1, 1 });

	EXPECT_TRUE(schema.has("account"));
	EXPECT_EQ(schema.read("account").name, "account");
	EXPECT_EQ(schema.names(), (std::set<std::string> { "account" }));
}

TEST(schema_test, a_name_the_schema_never_held_has_no_entry)
{
	EXPECT_FALSE(table::schema().read_entry("account").has_value());
	EXPECT_FALSE(table::schema().has("account"));
}

// The name stays as a tombstone, which is what tells a node that missed the delete from one that
// missed the create. Neither of them holds the table, and only one of them should take it back.
TEST(schema_test, a_dropped_table_leaves_a_name_that_is_not_live)
{
	table::schema schema = holding("account", record::version { 1, 1 });

	schema.remove("account", record::version { 1, 2 });

	EXPECT_FALSE(schema.has("account"));
	EXPECT_TRUE(schema.read_entry("account").has_value());
	EXPECT_FALSE(schema.read_entry("account")->live);
	EXPECT_TRUE(schema.names().empty());
}

TEST(schema_test, a_table_that_is_not_there_reads_as_table_not_found)
{
	EXPECT_EQ(table::schema().read("account").code, "table_not_found");
}

TEST(schema_test, takes_a_table_a_later_schema_has)
{
	table::schema schema;
	std::vector<table::schema::change> changed = schema.merge(holding("account", record::version { 1, 1 }));

	EXPECT_TRUE(schema.has("account"));
	ASSERT_EQ(changed.size(), 1u);
	EXPECT_EQ(changed[0].name, "account");
	EXPECT_TRUE(changed[0].live);
}

TEST(schema_test, takes_a_tombstone_stamped_after_the_table_it_holds)
{
	table::schema schema = holding("account", record::version { 1, 1 });
	table::schema dropped;

	dropped.remove("account", record::version { 1, 2 });

	std::vector<table::schema::change> changed = schema.merge(dropped);

	EXPECT_FALSE(schema.has("account"));
	ASSERT_EQ(changed.size(), 1u);
	EXPECT_FALSE(changed[0].live);
}

TEST(schema_test, keeps_a_table_against_a_tombstone_stamped_before_it)
{
	table::schema schema = holding("account", record::version { 2, 1 });
	table::schema dropped;

	dropped.remove("account", record::version { 1, 9 });

	EXPECT_TRUE(schema.merge(dropped).empty());
	EXPECT_TRUE(schema.has("account"));
}

// A term is the etcd revision behind a leader's claim, so it outranks any count: a leader that was
// replaced mid-flight issued its counts under the term it held.
TEST(schema_test, a_later_term_outranks_a_higher_count)
{
	table::schema schema = holding("account", record::version { 1, 900 });
	table::schema dropped;

	dropped.remove("account", record::version { 2, 1 });

	EXPECT_EQ(schema.merge(dropped).size(), 1u);
	EXPECT_FALSE(schema.has("account"));
}

// Two copies of one entry are the version this cannot order, and a schema meeting one keeps what
// it holds — the same rule a store meeting a record at its own version follows.
TEST(schema_test, keeps_what_it_holds_against_an_entry_at_its_own_version)
{
	table::schema schema = holding("account", record::version { 1, 1 });
	table::schema dropped;

	dropped.remove("account", record::version { 1, 1 });

	EXPECT_TRUE(schema.merge(dropped).empty());
	EXPECT_TRUE(schema.has("account"));
}

// **A live entry replacing a live one is not a change**, because one create carried to a node that
// had missed it is stamped again: dropping the column family on that would take the records of a
// table the cluster still has.
TEST(schema_test, a_later_table_over_one_it_holds_moves_the_document_and_nothing_else)
{
	table::schema schema = holding("account", record::version { 1, 1 });
	table::schema later;

	later.create(table::valid_table("account", std::vector<std::string> { }), record::version { 9, 9 });

	EXPECT_TRUE(schema.merge(later).empty());
	EXPECT_TRUE(schema.has("account"));
	EXPECT_EQ(schema.read_entry("account")->stamp.term, 9u);
}

// A name the other schema says nothing about is a node that is wrong about the schema rather than
// a delete, so a merge leaves it where it is.
TEST(schema_test, leaves_a_name_the_other_schema_does_not_carry)
{
	table::schema schema = holding("kept", record::version { 1, 1 });

	EXPECT_EQ(schema.merge(holding("account", record::version { 9, 9 })).size(), 1u);
	EXPECT_TRUE(schema.has("kept"));
	EXPECT_TRUE(schema.has("account"));
}

TEST(schema_test, a_schema_is_read_back_from_the_document_it_writes)
{
	table::schema schema = holding("account", record::version { 3, 4 });

	schema.remove("gone", record::version { 5, 6 });

	table::schema read = table::to_schema(boost::json::serialize(schema.json()));

	EXPECT_TRUE(read.has("account"));
	EXPECT_EQ(read.read("account").json, schema.read("account").json);
	EXPECT_EQ(read.read_entry("account")->stamp.term, 3u);
	EXPECT_EQ(read.read_entry("account")->stamp.count, 4u);

	ASSERT_TRUE(read.read_entry("gone").has_value());
	EXPECT_FALSE(read.read_entry("gone")->live);
	EXPECT_EQ(read.read_entry("gone")->stamp.term, 5u);
}

TEST(schema_test, a_document_that_is_not_a_schema_is_an_empty_one)
{
	EXPECT_TRUE(table::to_schema("").names().empty());
	EXPECT_TRUE(table::to_schema("[]").names().empty());
	EXPECT_TRUE(table::to_schema("{\"schema\":\"not an array\"}").names().empty());
}
