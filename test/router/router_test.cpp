#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#include "base64/base64.h"
#include "cluster/partition.h"
#include "cluster/fake_cluster.h"
#include "repository/fake_repository.h"
#include "router/router.h"
#include "url/url.h"

namespace
{
	router::request request(boost::beast::http::verb method, const std::string &target, const std::string &body)
	{
		return { method, url::split_path(target), url::query_string(target), body };
	}

	router::request get(const std::string &target)
	{
		return request(boost::beast::http::verb::get, target, "");
	}

	router::request put(const std::string &target, const std::string &body)
	{
		return request(boost::beast::http::verb::put, target, body);
	}

	router::request del(const std::string &target)
	{
		return request(boost::beast::http::verb::delete_, target, "");
	}

	std::string error_code(const router::response &response)
	{
		return std::string(response.json.at("error").as_object().at("code").as_string());
	}

	void create_table(router::router &router, const std::string &name)
	{
		router.route(put("/table/" + name, "{}"));
	}

	void write_record(router::router &router, const std::string &name, const std::string &key, const std::string &value)
	{
		router.route(put("/table/" + name + "/key/" + key, value));
	}

	const std::string here = "http://asyncdb-1:8080";

	// One node in no cluster: it holds every key and there is nobody to forward to, which is what
	// an instance nothing was told about runs as.
	cluster::fake_cluster lone_node()
	{
		return cluster::fake_cluster(here, std::vector<std::string>());
	}

	std::string every_partition()
	{
		cluster::partition_set held;

		held.set();

		return cluster::encode_partitions(held);
	}

	std::string only(const std::string &key)
	{
		cluster::partition_set held;

		held.set(cluster::partition_of(key));

		return cluster::encode_partitions(held);
	}

	// The version a record was stored with, which the API never carries: what a client is given is
	// the value, and the version is what one store compares with another.
	record::version stamp_of(
		const repository::fake_repository &repository,
		const std::string &name,
		const std::string &key)
	{
		scan::range whole;

		whole.is_valid = true;

		scan::page page = repository.scan_records(name, whole);

		for (size_t i = 0; i < page.records.size(); i++)
		{
			if (page.records[i].key == key)
			{
				return page.records[i].stamp;
			}
		}

		return record::version();
	}

	std::vector<std::string> keys(const router::response &response)
	{
		boost::json::array records = response.json.at("records").as_array();
		std::vector<std::string> keys;

		for (size_t i = 0; i < records.size(); i++)
		{
			keys.push_back(std::string(records[i].as_object().at("key").as_string()));
		}

		return keys;
	}
}

TEST(router_test, nonsense)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router::response response = router.route(get("/wibble"));

	EXPECT_EQ(response.status, boost::beast::http::status::not_found);
	EXPECT_EQ(error_code(response), "not_found");
}

TEST(router_test, health_says_whether_writes_are_stalled)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router::response response = router.route(get("/health"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.json.at("status"), "ok");
	EXPECT_EQ(response.json.at("write_stalled"), false);

	repository.stall();

	EXPECT_EQ(router.route(get("/health")).json.at("write_stalled"), true);
}

// A node holding less than it owns still serves what it has, so it answers health rather than
// dropping out of the load balancer: what it cannot do is say a key is missing.
TEST(router_test, health_says_whether_this_node_holds_less_than_it_owns)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	EXPECT_EQ(router.route(get("/health")).json.at("incomplete"), false);

	router.is_incomplete(true);

	router::response response = router.route(get("/health"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.json.at("status"), "ok");
	EXPECT_EQ(response.json.at("incomplete"), true);
}

// The one thing in this document a load balancer can read, which is why it is the status and not
// only a field: a node taking no writes goes on holding what it holds and answering its peers, so
// nothing else about it says to stop choosing it.
TEST(router_test, health_refuses_the_check_of_a_node_that_can_order_no_write)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	EXPECT_EQ(router.route(get("/health")).json.at("unled"), false);
	EXPECT_EQ(router.route(get("/health")).status, boost::beast::http::status::ok);

	alone.unled();

	router::response response = router.route(get("/health"));

	EXPECT_EQ(response.status, boost::beast::http::status::service_unavailable);
	EXPECT_EQ(response.json.at("unled"), true);

	// And the document is still the document, because the status is for the load balancer and the
	// fields are for whoever is reading the node.
	EXPECT_EQ(response.json.at("status"), "ok");
	EXPECT_EQ(response.json.at("write_stalled"), false);
}

TEST(router_test, list_no_tables)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router::response response = router.route(get("/table"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.json.at("tables").as_array().size(), 0);
}

TEST(router_test, create_a_table)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router::response response = router.route(put("/table/account", "{}"));

	EXPECT_EQ(response.status, boost::beast::http::status::created);
	EXPECT_EQ(response.json.at("name"), "account");
	EXPECT_TRUE(repository.has_table("account"));
}

TEST(router_test, create_a_table_with_no_body_at_all)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router::response response = router.route(put("/table/account", ""));

	EXPECT_EQ(response.status, boost::beast::http::status::created);
	EXPECT_EQ(response.json.at("dependencies").as_array().size(), 0);
}

TEST(router_test, creating_the_same_table_again_changes_nothing)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router.route(put("/table/account", "{}"));

	router::response response = router.route(put("/table/account", "{\"dependencies\":[]}"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.json.at("name"), "account");
}

TEST(router_test, fail_to_create_a_table_that_exists_with_different_options)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	router::response response = router.route(put("/table/account", "{\"dependencies\":[\"account\"]}"));

	EXPECT_EQ(response.status, boost::beast::http::status::conflict);
	EXPECT_EQ(error_code(response), "table_exists");
}

TEST(router_test, fail_to_create_a_table_with_an_invalid_name)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router::response response = router.route(put("/table/An%2FAccount", "{}"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_table_name");
	EXPECT_FALSE(repository.has_table("An Account"));
}

TEST(router_test, fail_to_create_a_table_from_a_body_that_is_not_json)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router::response response = router.route(put("/table/account", "not json"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_body");
	EXPECT_FALSE(repository.has_table("account"));
}

TEST(router_test, fail_to_create_a_table_from_a_body_that_is_not_an_object)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router::response response = router.route(put("/table/account", "[]"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_body");
	EXPECT_FALSE(repository.has_table("account"));
}

TEST(router_test, fail_to_create_a_table_that_depends_on_one_that_is_not_there)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router::response response = router.route(put("/table/transaction", "{\"dependencies\":[\"account\"]}"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "dependency_not_found");
	EXPECT_FALSE(repository.has_table("transaction"));
}

TEST(router_test, list_the_tables_and_their_dependencies)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	router.route(put("/table/transaction", "{\"dependencies\":[\"account\"]}"));

	router::response response = router.route(get("/table"));

	boost::json::array tables = response.json.at("tables").as_array();

	EXPECT_EQ(tables.size(), 2);
	EXPECT_EQ(tables[0].as_object().at("name"), "account");
	EXPECT_EQ(tables[1].as_object().at("name"), "transaction");
	EXPECT_EQ(tables[1].as_object().at("dependencies").as_array()[0], "account");
}

TEST(router_test, inspect_a_table)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	router::response response = router.route(get("/table/account"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.json.at("name"), "account");
	EXPECT_EQ(response.json.at("dependencies").as_array().size(), 0);
}

TEST(router_test, fail_to_inspect_a_table_that_is_not_there)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router::response response = router.route(get("/table/account"));

	EXPECT_EQ(response.status, boost::beast::http::status::not_found);
	EXPECT_EQ(error_code(response), "table_not_found");
}

TEST(router_test, delete_a_table_and_its_data)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "4821", "Eleanor Whitmore");

	router::response response = router.route(del("/table/account"));

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	EXPECT_EQ(response.content_type, "");
	EXPECT_FALSE(repository.has_table("account"));

	create_table(router, "account");

	EXPECT_EQ(router.route(get("/table/account/key")).json.at("records").as_array().size(), 0);
}

TEST(router_test, fail_to_delete_a_table_that_is_not_there)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router::response response = router.route(del("/table/account"));

	EXPECT_EQ(response.status, boost::beast::http::status::not_found);
	EXPECT_EQ(error_code(response), "table_not_found");
}

TEST(router_test, write_then_read_a_record)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	router::response written = router.route(put("/table/account/key/4821", "Eleanor Whitmore"));

	EXPECT_EQ(written.status, boost::beast::http::status::no_content);

	router::response response = router.route(get("/table/account/key/4821"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.content_type, "text/plain; charset=utf-8");
	EXPECT_EQ(response.text, "Eleanor Whitmore");
}

TEST(router_test, a_missing_key_and_an_empty_value_are_told_apart_by_the_status)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "4821", "");

	router::response empty = router.route(get("/table/account/key/4821"));

	EXPECT_EQ(empty.status, boost::beast::http::status::ok);
	EXPECT_EQ(empty.text, "");

	router::response missing = router.route(get("/table/account/key/7203"));

	EXPECT_EQ(missing.status, boost::beast::http::status::not_found);
	EXPECT_EQ(missing.content_type, "");
}

TEST(router_test, a_value_is_kept_as_the_bytes_it_was_given)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "4821", "{\"firstName\":\"Eleanor\"");

	EXPECT_EQ(router.route(get("/table/account/key/4821")).text, "{\"firstName\":\"Eleanor\"");
}

TEST(router_test, delete_a_record)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "4821", "Eleanor Whitmore");

	router::response response = router.route(del("/table/account/key/4821"));

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	EXPECT_EQ(router.route(get("/table/account/key/4821")).status, boost::beast::http::status::not_found);
}

TEST(router_test, deleting_a_record_that_is_not_there_is_a_no_op)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	EXPECT_EQ(router.route(del("/table/account/key/4821")).status, boost::beast::http::status::no_content);
}

TEST(router_test, overwrite_a_record)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "4821", "Eleanor Whitmore");
	write_record(router, "account", "4821", "Eleanor Ashby");

	EXPECT_EQ(router.route(get("/table/account/key/4821")).text, "Eleanor Ashby");
}

TEST(router_test, fail_to_read_a_record_of_a_table_that_is_not_there)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router::response response = router.route(get("/table/account/key/4821"));

	EXPECT_EQ(response.status, boost::beast::http::status::not_found);
	EXPECT_EQ(error_code(response), "table_not_found");
}

TEST(router_test, fail_to_read_a_key_that_is_not_valid_utf8)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	router::response response = router.route(get("/table/account/key/%C3%28"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_key_encoding");
}

TEST(router_test, fail_to_write_a_key_that_is_too_large)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	router::response response =
		router.route(put("/table/account/key/" + std::string(record::max_key_size + 1, 'k'), "a value"));

	EXPECT_EQ(response.status, boost::beast::http::status::payload_too_large);
	EXPECT_EQ(error_code(response), "key_too_large");
}

TEST(router_test, fail_to_write_a_value_that_is_too_large)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	router::response response =
		router.route(put("/table/account/key/4821", std::string(record::max_value_size + 1, 'v')));

	EXPECT_EQ(response.status, boost::beast::http::status::payload_too_large);
	EXPECT_EQ(error_code(response), "value_too_large");
}

TEST(router_test, scan_a_table_in_key_order)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "user%3A7203", "Marcus Hale");
	write_record(router, "account", "user%3A4821", "Eleanor Whitmore");
	write_record(router, "account", "order%3A1", "an order");

	router::response response = router.route(get("/table/account/key"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(keys(response), (std::vector<std::string> { "order:1", "user:4821", "user:7203" }));
	EXPECT_EQ(response.json.at("records").as_array()[1].as_object().at("value"), "Eleanor Whitmore");
	EXPECT_FALSE(response.json.contains("next"));
}

TEST(router_test, scan_a_prefix)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "user%3A7203", "Marcus Hale");
	write_record(router, "account", "user%3A4821", "Eleanor Whitmore");
	write_record(router, "account", "order%3A1", "an order");

	router::response response = router.route(get("/table/account/key?prefix=user%3A"));

	EXPECT_EQ(keys(response), (std::vector<std::string> { "user:4821", "user:7203" }));
}

TEST(router_test, scan_backwards)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "1", "one");
	write_record(router, "account", "2", "two");
	write_record(router, "account", "3", "three");

	router::response response = router.route(get("/table/account/key?reverse=true"));

	EXPECT_EQ(keys(response), (std::vector<std::string> { "3", "2", "1" }));
}

TEST(router_test, scan_keys_only)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "4821", "Eleanor Whitmore");

	router::response response = router.route(get("/table/account/key?values=false"));

	boost::json::object record = response.json.at("records").as_array()[0].as_object();

	EXPECT_EQ(record.at("key"), "4821");
	EXPECT_FALSE(record.contains("value"));
}

TEST(router_test, page_through_a_scan_with_a_cursor)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "1", "one");
	write_record(router, "account", "2", "two");
	write_record(router, "account", "3", "three");

	router::response first = router.route(get("/table/account/key?limit=2"));

	EXPECT_EQ(keys(first), (std::vector<std::string> { "1", "2" }));
	EXPECT_TRUE(first.json.contains("next"));

	std::string cursor = std::string(first.json.at("next").as_string());
	router::response second = router.route(get("/table/account/key?limit=2&cursor=" + cursor));

	EXPECT_EQ(keys(second), (std::vector<std::string> { "3" }));

	// The absence of a cursor means the range is exhausted.
	EXPECT_FALSE(second.json.contains("next"));
}

TEST(router_test, page_backwards_through_a_scan)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "1", "one");
	write_record(router, "account", "2", "two");
	write_record(router, "account", "3", "three");

	router::response first = router.route(get("/table/account/key?limit=2&reverse=true"));

	EXPECT_EQ(keys(first), (std::vector<std::string> { "3", "2" }));

	std::string cursor = std::string(first.json.at("next").as_string());
	router::response second = router.route(get("/table/account/key?limit=2&reverse=true&cursor=" + cursor));

	EXPECT_EQ(keys(second), (std::vector<std::string> { "1" }));
}

// The page a client is given is bounded in bytes, and the cursor is how the rest is asked for —
// so a table of large values pages rather than answering with a response the node cannot build.
TEST(router_test, page_through_a_scan_of_values_too_large_to_send_at_once)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	std::string value(2 * 1024 * 1024, 'v');

	for (size_t i = 0; i < 6; i++)
	{
		write_record(router, "account", std::to_string(i), value);
	}

	router::response first = router.route(get("/table/account/key"));

	EXPECT_EQ(keys(first), (std::vector<std::string> { "0", "1", "2" }));
	EXPECT_TRUE(first.json.contains("next"));

	std::string cursor = std::string(first.json.at("next").as_string());
	router::response second = router.route(get("/table/account/key?cursor=" + cursor));

	EXPECT_EQ(keys(second), (std::vector<std::string> { "3", "4", "5" }));
	EXPECT_FALSE(second.json.contains("next"));
}

TEST(router_test, fail_to_scan_with_a_cursor_this_instance_did_not_issue)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	router::response response =
		router.route(get("/table/account/key?cursor=" + scan::encode_cursor("1", "another instance")));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_cursor");
}

TEST(router_test, fail_to_scan_a_range_that_is_not_below_its_end)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	router::response response = router.route(get("/table/account/key?from=b&to=a"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_range");
}

TEST(router_test, fail_to_scan_a_table_that_is_not_there)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	EXPECT_EQ(error_code(router.route(get("/table/account/key"))), "table_not_found");
}

TEST(router_test, delete_a_range)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "user%3A2019", "a user");
	write_record(router, "account", "user%3A2020", "another user");
	write_record(router, "account", "order%3A1", "an order");

	router::response response = router.route(del("/table/account/key?prefix=user%3A"));

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	EXPECT_EQ(keys(router.route(get("/table/account/key"))), (std::vector<std::string> { "order:1" }));
}

TEST(router_test, refuse_to_delete_a_range_that_names_no_range)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "4821", "Eleanor Whitmore");

	router::response response = router.route(del("/table/account/key"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_range");
	EXPECT_EQ(keys(router.route(get("/table/account/key"))).size(), 1);
}

TEST(router_test, a_method_that_is_not_a_method_of_the_route)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	router::response response = router.route(request(boost::beast::http::verb::post, "/table/account", "{}"));

	EXPECT_EQ(response.status, boost::beast::http::status::method_not_allowed);
	EXPECT_EQ(error_code(response), "method_not_allowed");
}

TEST(router_test, a_path_below_a_key_is_not_a_route)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	EXPECT_EQ(router.route(get("/table/account/key/4821/name")).status, boost::beast::http::status::not_found);
	EXPECT_EQ(router.route(get("/table/account/wibble")).status, boost::beast::http::status::not_found);
}

namespace
{
	const std::string there = "http://asyncdb-2:8080";

	const std::string elsewhere = "http://asyncdb-3:8080";

	cluster::fake_cluster two_nodes()
	{
		return cluster::fake_cluster(here, { here, there });
	}

	const std::string partner = "http://asyncdb-4:8080";

	// Two zones of two nodes: this node and its partner split the copy their zone holds, and the
	// other two split the copy theirs holds.
	cluster::fake_cluster paired_zones()
	{
		return cluster::fake_cluster(here, std::vector<cluster::member> {
			cluster::member { here, "a" },
			cluster::member { partner, "a" },
			cluster::member { there, "b" },
			cluster::member { elsewhere, "b" }
		});
	}

	// Three nodes, one in each zone, which is a cluster holding a copy of every record on every
	// one of them.
	cluster::fake_cluster three_zones()
	{
		return cluster::fake_cluster(here, std::vector<cluster::member> {
			cluster::member { here, "a" },
			cluster::member { there, "b" },
			cluster::member { elsewhere, "c" }
		});
	}

	router::response page(const boost::json::array &records, bool has_more)
	{
		boost::json::object body { { "records", records } };

		if (has_more)
		{
			body["next"] = "a cursor of the other node's";
		}

		return router::json_response(boost::beast::http::status::ok, body);
	}

	boost::json::object record_json(const std::string &key, const std::string &value)
	{
		return boost::json::object { { "key", key }, { "value", value } };
	}

	boost::json::object record_json(const std::string &key, const std::string &sort, const std::string &value)
	{
		return boost::json::object { { "key", key }, { "sort", sort }, { "value", value } };
	}

	std::string cursor_key(const router::response &response)
	{
		std::string decoded = base64::decode(std::string(response.json.at("next").as_string())).value_or("");

		return std::string(boost::json::parse(decoded).as_object().at("k").as_string());
	}
}

TEST(router_cluster_test, read_a_record_from_the_node_that_owns_the_key)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.owns("4821", there);
	nodes.answer(there, router::text_response(boost::beast::http::status::ok, "a value"));

	router::response response = router.route(get("/table/account/key/4821"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.text, "a value");

	// The request travels as it stands, so the node that owns the key answers the same question.
	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].first, there);
	EXPECT_EQ(nodes.sent()[0].second.method, boost::beast::http::verb::get);
	EXPECT_EQ(nodes.sent()[0].second.path, (std::vector<std::string> { "table", "account", "key", "4821" }));
}

TEST(router_cluster_test, read_a_record_this_node_owns_without_a_hop)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "4821", "a value");
	nodes.forget();

	router::response response = router.route(get("/table/account/key/4821"));

	EXPECT_EQ(response.text, "a value");
	EXPECT_TRUE(nodes.sent().empty());
}

TEST(router_cluster_test, write_a_record_to_the_node_that_owns_the_key)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.owns("4821", there);

	router::response response = router.route(put("/table/account/key/4821", "a value"));

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].second.body, "a value");
	EXPECT_FALSE(repository.read_record("account", "4821").has_value());
}

TEST(router_cluster_test, delete_a_record_on_the_node_that_owns_the_key)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.owns("4821", there);

	router::response response = router.route(del("/table/account/key/4821"));

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].second.method, boost::beast::http::verb::delete_);
}

// The node that was sent the request is the node that answers it, whatever it makes of the
// membership, so two nodes that disagree for a moment cannot bounce a request between them.
TEST(router_cluster_test, serve_a_forwarded_record_where_it_stands)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.owns("4821", there);

	router::request forwarded = put("/table/account/key/4821", "a value");

	forwarded.forwarded = true;

	EXPECT_EQ(router.route(forwarded).status, boost::beast::http::status::no_content);
	EXPECT_TRUE(nodes.sent().empty());
	EXPECT_EQ(repository.read_record("account", "4821"), "a value");
}

TEST(router_cluster_test, fail_to_write_to_a_table_that_is_not_there_without_a_hop)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.owns("4821", there);

	router::response response = router.route(put("/table/account/key/4821", "a value"));

	EXPECT_EQ(error_code(response), "table_not_found");
	EXPECT_TRUE(nodes.sent().empty());
}

// Every zone holds a copy, so a write is not done until every copy has taken it — this node's own
// included.
TEST(router_cluster_test, write_a_record_to_every_node_that_holds_a_copy)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { here, there, elsewhere });

	router::response response = router.route(put("/table/account/key/4821", "a value"));

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	EXPECT_EQ(repository.read_record("account", "4821"), "a value");

	ASSERT_EQ(nodes.sent().size(), 2u);
	EXPECT_EQ(nodes.sent()[0].first, there);
	EXPECT_EQ(nodes.sent()[0].second.body, "a value");
	EXPECT_EQ(nodes.sent()[1].first, elsewhere);
	EXPECT_EQ(nodes.sent()[1].second.body, "a value");
}

TEST(router_cluster_test, write_a_record_to_every_copy_when_this_node_holds_none)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { there, elsewhere });

	router::response response = router.route(put("/table/account/key/4821", "a value"));

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	EXPECT_FALSE(repository.read_record("account", "4821").has_value());
	EXPECT_EQ(nodes.sent().size(), 2u);
}

TEST(router_cluster_test, delete_a_record_from_every_node_that_holds_a_copy)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "4821", "a value");
	nodes.forget();
	nodes.copies("4821", { here, there, elsewhere });

	router::response response = router.route(del("/table/account/key/4821"));

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	EXPECT_FALSE(repository.read_record("account", "4821").has_value());

	ASSERT_EQ(nodes.sent().size(), 2u);
	EXPECT_EQ(nodes.sent()[0].second.method, boost::beast::http::verb::delete_);
	EXPECT_EQ(nodes.sent()[1].second.method, boost::beast::http::verb::delete_);
}

// A copy that refuses is a record that is not in every zone, and saying so is what lets the client
// write it again — which is safe, because writing a record twice is writing it once.
TEST(router_cluster_test, fail_to_write_a_record_a_copy_refuses)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { here, there, elsewhere });
	nodes.answer(there, router::error_response("write_stalled", "Writes are stalled."));

	EXPECT_EQ(error_code(router.route(put("/table/account/key/4821", "a value"))), "write_stalled");

	// The copies are asked at once, so the zone behind the one that refused was asked as well.
	// That is a request that need not have been sent rather than a wrong answer: the client is
	// told to run the whole write again, and writing a record twice is writing it once.
	EXPECT_EQ(nodes.sent().size(), 2u);
}

// The copies are asked at once, so more than one of them can refuse. The one reported is the
// first in the order they were asked rather than whichever answered first, so that a write is
// refused with the same reason however the answers happen to come back.
TEST(router_cluster_test, report_the_first_copy_to_refuse_a_write)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { here, there, elsewhere });
	nodes.answer(there, router::error_response("write_stalled", "Writes are stalled."));
	nodes.answer(elsewhere, router::error_response("storage_error", "The store failed."));

	EXPECT_EQ(error_code(router.route(put("/table/account/key/4821", "a value"))), "write_stalled");
	EXPECT_EQ(nodes.sent().size(), 2u);
}

// The reason for keeping a copy in every zone: a zone that is gone is a copy to pass over.
TEST(router_cluster_test, read_a_record_from_the_next_copy_when_a_node_does_not_answer)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { there, elsewhere });
	nodes.answer(there, router::error_response("storage_error", "Node \"" + there + "\" did not answer."));
	nodes.answer(elsewhere, router::text_response(boost::beast::http::status::ok, "a value"));

	router::response response = router.route(get("/table/account/key/4821"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.text, "a value");
	EXPECT_EQ(nodes.sent().size(), 2u);
}

// A key that is not there is not there in any zone, because every zone is written before a write
// is answered, so a 404 is an answer and not a copy to pass over.
TEST(router_cluster_test, take_a_missing_key_from_the_first_copy_that_answers)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { there, elsewhere });
	nodes.answer(there, router::empty_response(boost::beast::http::status::not_found));

	EXPECT_EQ(router.route(get("/table/account/key/4821")).status, boost::beast::http::status::not_found);
	EXPECT_EQ(nodes.sent().size(), 1u);
}

// A node that was replaced, or a zone that came back, is the owner of keys in its own zone and
// holds none of them. The copies in the other zones are what stop that reading as a record that
// was never written.
TEST(router_cluster_test, read_a_record_from_another_zone_when_this_node_has_none_of_it)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { here, there });
	nodes.answer(there, router::text_response(boost::beast::http::status::ok, "a value"));

	router::response response = router.route(get("/table/account/key/4821"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.text, "a value");
	EXPECT_EQ(nodes.sent().size(), 1u);
}

TEST(router_cluster_test, answer_a_key_no_zone_holds_as_missing)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { here, there });
	nodes.answer(there, router::empty_response(boost::beast::http::status::not_found));

	EXPECT_EQ(router.route(get("/table/account/key/4821")).status, boost::beast::http::status::not_found);
}

// A forwarded read is served where it stands, so a node that was asked because it holds a copy
// answers out of its own store and does not ask a third node about it.
TEST(router_cluster_test, answer_a_forwarded_read_of_a_key_this_node_has_none_of)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { here, there });

	router::request forwarded = get("/table/account/key/4821");

	forwarded.forwarded = true;

	EXPECT_EQ(router.route(forwarded).status, boost::beast::http::status::not_found);
	EXPECT_TRUE(nodes.sent().empty());
}

// A rebuild that did not read the whole of this node's share leaves it unable to tell a key that
// was never written from one it never received, so the node that asked is told to ask elsewhere
// rather than handed a miss it would believe.
TEST(router_cluster_test, refuse_to_call_a_key_missing_when_this_node_holds_less_than_it_owns)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { here, there });

	router::request forwarded = get("/table/account/key/4821");

	forwarded.forwarded = true;

	router.is_incomplete(true);

	router::response response = router.route(forwarded);

	EXPECT_EQ(response.status, boost::beast::http::status::service_unavailable);
	EXPECT_EQ(error_code(response), "node_incomplete");
	EXPECT_TRUE(nodes.sent().empty());
}

// The tables are the first thing a rebuild reads, so a node that came up short may hold none of
// them: an unknown table is the same answer as an unknown key.
TEST(router_cluster_test, refuse_to_call_a_table_missing_when_this_node_holds_less_than_it_owns)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	router.is_incomplete(true);

	EXPECT_EQ(error_code(router.route(get("/table/account/key/4821"))), "node_incomplete");
}

// The record is here, so nothing about the rest of the share bears on it.
TEST(router_cluster_test, answer_a_key_this_node_holds_while_it_holds_less_than_it_owns)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { here, there });
	repository.write_record("account", record::valid_record("4821", "Robert"));

	router::request forwarded = get("/table/account/key/4821");

	forwarded.forwarded = true;

	router.is_incomplete(true);

	router::response response = router.route(forwarded);

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.text, "Robert");
}

TEST(router_cluster_test, fail_to_read_a_record_no_copy_of_which_answers)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { there, elsewhere });
	nodes.answer(there, router::error_response("storage_error", "Node \"" + there + "\" did not answer."));
	nodes.answer(elsewhere, router::error_response("storage_error", "Node \"" + elsewhere + "\" did not answer."));

	EXPECT_EQ(error_code(router.route(get("/table/account/key/4821"))), "storage_error");
	EXPECT_EQ(nodes.sent().size(), 2u);
}

// Writes to a partition are ordered by the node that leads it, so a write that lands anywhere else
// travels there whole rather than being fanned out from where it landed.
TEST(router_cluster_test, write_a_record_through_the_node_that_leads_its_partition)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { here, partner, there });
	nodes.led_by("4821", there, 41);

	router::response response = router.route(put("/table/account/key/4821", "a value"));

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);

	// One hop to the leader, and no copy written from here: the leader decides the order and
	// writes every copy itself.
	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].first, there);
	EXPECT_EQ(nodes.sent()[0].second.body, "a value");
	EXPECT_FALSE(repository.read_record("account", "4821").has_value());
}

// This node leads it, so this is where the order is decided — and every copy is told the term it
// was decided in.
TEST(router_cluster_test, order_a_write_of_a_partition_this_node_leads)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { here, partner });
	nodes.led_by("4821", here, 41);

	router::response response = router.route(put("/table/account/key/4821", "a value"));

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	EXPECT_EQ(repository.read_record("account", "4821"), "a value");

	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].first, partner);
	EXPECT_EQ(nodes.sent()[0].second.term, 41);
}

// The leader stamps the write with the term it ordered it in and a count of its own, and both
// halves travel: the copies of one write are one record and have to be able to say so.
TEST(router_cluster_test, stamp_a_write_with_the_version_it_was_ordered_in)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { here, partner });
	nodes.led_by("4821", here, 41);

	router.route(put("/table/account/key/4821", "a value"));

	record::version stamped = stamp_of(repository, "account", "4821");

	EXPECT_EQ(stamped.term, 41u);
	EXPECT_NE(stamped.count, 0u);

	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].second.term, 41);
	EXPECT_EQ(nodes.sent()[0].second.count, stamped.count);
}

// The count is what orders two writes the same leader ordered, which a term of its own cannot: a
// term stands for as long as the claim behind it does.
TEST(router_cluster_test, count_a_write_after_the_one_before_it)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.copies("4821", { here, partner });
	nodes.led_by("4821", here, 41);

	router.route(put("/table/account/key/4821", "one"));

	uint64_t first = stamp_of(repository, "account", "4821").count;

	router.route(put("/table/account/key/4821", "two"));

	EXPECT_GT(stamp_of(repository, "account", "4821").count, first);
}

// A copy applies the version it was given rather than making one of its own, or the copies of one
// write would be records that nothing could tell apart from two writes.
TEST(router_cluster_test, apply_the_version_a_forwarded_write_was_ordered_in)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();

	router::request forwarded = put("/table/account/key/4821", "a value");

	forwarded.forwarded = true;
	forwarded.term = 60;
	forwarded.count = 7;

	EXPECT_EQ(router.route(forwarded).status, boost::beast::http::status::no_content);

	record::version stamped = stamp_of(repository, "account", "4821");

	EXPECT_EQ(stamped.term, 60u);
	EXPECT_EQ(stamped.count, 7u);
}

// An instance nothing leads writes where it always has, and still counts: a store that joins a
// cluster later is one whose records are weighed against another node's.
TEST(router_test, count_a_write_no_leader_ordered)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	router.route(put("/table/account/key/4821", "a value"));

	record::version stamped = stamp_of(repository, "account", "4821");

	EXPECT_EQ(stamped.term, 0u);
	EXPECT_NE(stamped.count, 0u);
}

// A partition nothing leads has nowhere to order a write, and saying so is what makes the client
// run it again once an election has happened.
TEST(router_cluster_test, refuse_a_write_of_a_partition_nothing_leads)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.led_by_nobody("4821");

	router::response response = router.route(put("/table/account/key/4821", "a value"));

	EXPECT_EQ(error_code(response), "no_leader");
	EXPECT_EQ(response.status, boost::beast::http::status::service_unavailable);
	EXPECT_TRUE(nodes.sent().empty());
	EXPECT_FALSE(repository.read_record("account", "4821").has_value());
}

// The fence. A leader that has lost its lease and does not know it is a leader whose writes the
// copies have already moved past.
TEST(router_cluster_test, refuse_a_forwarded_write_ordered_in_a_term_that_has_passed)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.applied("4821", 60);

	router::request forwarded = put("/table/account/key/4821", "a value");

	forwarded.forwarded = true;
	forwarded.term = 41;

	router::response response = router.route(forwarded);

	EXPECT_EQ(error_code(response), "stale_leader");
	EXPECT_EQ(response.status, boost::beast::http::status::conflict);
	EXPECT_FALSE(repository.read_record("account", "4821").has_value());
}

// Two nodes disagreeing about who leads a partition must not bounce a write between them, so a
// write sent here to be ordered, at a node that does not order it, is refused.
TEST(router_cluster_test, refuse_a_write_sent_here_to_be_ordered_that_this_node_does_not_lead)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.led_by("4821", there, 41);

	router::request forwarded = put("/table/account/key/4821", "a value");

	forwarded.forwarded = true;

	EXPECT_EQ(error_code(router.route(forwarded)), "no_leader");
	EXPECT_TRUE(nodes.sent().empty());
	EXPECT_FALSE(repository.read_record("account", "4821").has_value());
}

TEST(router_cluster_test, apply_a_forwarded_write_ordered_in_the_term_that_stands)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.applied("4821", 60);

	router::request forwarded = put("/table/account/key/4821", "a value");

	forwarded.forwarded = true;
	forwarded.term = 60;

	EXPECT_EQ(router.route(forwarded).status, boost::beast::http::status::no_content);
	EXPECT_EQ(repository.read_record("account", "4821"), "a value");
	EXPECT_TRUE(nodes.sent().empty());
}

// A delete is a write like any other, and it is ordered by the same node.
TEST(router_cluster_test, order_a_delete_through_the_leader)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.copies("4821", { here, partner });
	nodes.led_by("4821", there, 41);

	EXPECT_EQ(router.route(del("/table/account/key/4821")).status, boost::beast::http::status::no_content);

	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].first, there);
	EXPECT_EQ(nodes.sent()[0].second.method, boost::beast::http::verb::delete_);
}

namespace
{
	// The zones of paired_zones, with a fan out slow enough to be caught overlapping another and a
	// count of the most writes that were ever inside one at once.
	class counting_cluster : public cluster::fake_cluster
	{
		mutable std::mutex counting;

		mutable size_t inside = 0;

		mutable size_t most = 0;

		void enter() const
		{
			std::lock_guard<std::mutex> lock(counting);

			inside++;
			most = std::max(most, inside);
		}

		void leave() const
		{
			std::lock_guard<std::mutex> lock(counting);

			inside--;
		}

	public:
		counting_cluster():
			fake_cluster(here, std::vector<::cluster::member> {
				::cluster::member { here, "a" },
				::cluster::member { partner, "a" },
				::cluster::member { there, "b" },
				::cluster::member { elsewhere, "b" }
			})
		{
		}

		// The fan out the ordering lock is held across, which is the whole of what these tests
		// watch. It answers for the nodes rather than recording what they were asked, because a
		// fake_cluster remembers that in a vector and two threads remembering at once is a race
		// of the test's own making.
		std::optional<router::response> send_all(
			const std::vector<std::string> &,
			const router::request &) const override
		{
			enter();
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			leave();

			return std::optional<router::response>();
		}

		size_t most_at_once() const
		{
			std::lock_guard<std::mutex> lock(counting);

			return most;
		}
	};

	void write_together(router::router &router, const std::vector<std::string> &keys)
	{
		std::vector<std::thread> writers;

		for (size_t i = 0; i < keys.size(); i++)
		{
			writers.push_back(std::thread(write_record, std::ref(router), "account", keys[i], "a value"));
		}

		for (size_t i = 0; i < writers.size(); i++)
		{
			writers[i].join();
		}
	}
}

// Two clients writing one key at one leader are ordered by it: the fan out of the second does not
// start until every copy has taken the first, so no copy is ever applying two writes of one key at
// a time and they cannot settle in two orders.
TEST(router_cluster_test, order_concurrent_writes_of_one_key)
{
	repository::fake_repository repository;
	counting_cluster nodes;
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.copies("4821", { here, partner });
	nodes.led_by("4821", here, 41);

	write_together(router, { "4821", "4821", "4821", "4821" });

	EXPECT_EQ(nodes.most_at_once(), 1u);
	EXPECT_EQ(repository.read_record("account", "4821"), "a value");
}

// The locks are striped and not one lock. Writes of different keys order against nothing and run
// at once, which is what keeps every write on a node from queueing behind the busiest key on it.
TEST(router_cluster_test, write_different_keys_at_once)
{
	repository::fake_repository repository;
	counting_cluster nodes;
	router::router router(repository, nodes);
	std::vector<std::string> keys;

	create_table(router, "account");

	for (size_t i = 0; i < 8; i++)
	{
		keys.push_back("482" + std::to_string(i));

		// A leader holding no copy of the key, so that a write fans out and touches no store:
		// two threads writing one fake_repository is a race of the test's own making.
		nodes.copies(keys.back(), { partner, there });
		nodes.led_by(keys.back(), here, 41);
	}

	write_together(router, keys);

	EXPECT_GT(nodes.most_at_once(), 1u);
}

// A read is not ordered by anybody: it is answered by a copy, and the leader is not in its way.
TEST(router_cluster_test, read_a_record_without_asking_the_leader)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "4821", "a value");
	nodes.forget();
	nodes.led_by("4821", there, 41);

	EXPECT_EQ(router.route(get("/table/account/key/4821")).text, "a value");
	EXPECT_TRUE(nodes.sent().empty());
}

TEST(router_cluster_test, create_a_table_on_every_node)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	router::response response = router.route(put("/table/account", "{}"));

	EXPECT_EQ(response.status, boost::beast::http::status::created);
	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].first, there);
	EXPECT_EQ(nodes.sent()[0].second.path, (std::vector<std::string> { "table", "account" }));
	EXPECT_EQ(nodes.sent()[0].second.body, "{}");
}

// Every node holds every table, so the list is answered out of this node's own store.
TEST(router_cluster_test, list_the_tables_without_asking_another_node)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();

	EXPECT_EQ(router.route(get("/table")).json.at("tables").as_array().size(), 1u);
	EXPECT_TRUE(nodes.sent().empty());
}

TEST(router_cluster_test, fail_to_create_a_table_a_node_refuses)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.answer(there, router::error_response("table_exists", "A table named \"account\" exists."));

	router::response response = router.route(put("/table/account", "{}"));

	EXPECT_EQ(error_code(response), "table_exists");
}

TEST(router_cluster_test, create_a_forwarded_table_without_passing_it_on)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	router::request forwarded = put("/table/account", "{}");

	forwarded.forwarded = true;

	EXPECT_EQ(router.route(forwarded).status, boost::beast::http::status::created);
	EXPECT_TRUE(nodes.sent().empty());
	EXPECT_TRUE(repository.has_table("account"));
}

TEST(router_cluster_test, delete_a_table_on_every_node)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();

	router::response response = router.route(del("/table/account"));

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].second.method, boost::beast::http::verb::delete_);
}

// A node that never had the table has nothing to say about a deletion the rest of the cluster is
// carrying out, so it agrees rather than answering that it is not there.
TEST(router_cluster_test, agree_to_a_forwarded_deletion_of_a_table_that_is_not_there)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	router::request forwarded = del("/table/account");

	forwarded.forwarded = true;

	EXPECT_EQ(router.route(forwarded).status, boost::beast::http::status::no_content);
	EXPECT_EQ(router.route(del("/table/account")).status, boost::beast::http::status::not_found);
}

// The client's own delete, forwarded to the node that leads the tables because it landed on one
// that does not. It carries no term, so it is the request to order and not an order to apply, and
// a table that is not there is a 404 for the client rather than the leader agreeing with itself.
TEST(router_cluster_test, refuse_a_deletion_forwarded_to_the_leader_of_a_table_that_is_not_there)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.led_by(cluster::table_key, here, 41);

	router::request forwarded = del("/table/account");

	forwarded.forwarded = true;

	EXPECT_EQ(error_code(router.route(forwarded)), "table_not_found");
}

// The leader deletes its own copy before it carries the order out, so a delete that one node
// refused is a table the leader no longer has: the order goes out again rather than stopping at
// the leader's own 404, which is what makes running the request again the remedy for a delete as
// well as for a create.
TEST(router_cluster_test, carry_a_delete_of_a_table_that_is_not_here_to_the_other_nodes)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.led_by(cluster::table_key, here, 41);

	EXPECT_EQ(error_code(router.route(del("/table/account"))), "table_not_found");

	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].first, there);
	EXPECT_EQ(nodes.sent()[0].second.method, boost::beast::http::verb::delete_);
	EXPECT_EQ(nodes.sent()[0].second.term, 41);
}

// A node that refused the order is still holding the table, so what the client is told is the
// refusal it has to run the request again for and not that the table is gone.
TEST(router_cluster_test, report_a_node_that_refuses_a_delete_of_a_table_that_is_not_here)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.led_by(cluster::table_key, here, 41);
	nodes.answer(there, router::error_response("storage_error", "Node \"" + there + "\" did not answer."));

	EXPECT_EQ(error_code(router.route(del("/table/account"))), "storage_error");
}

// A node that came up short of its share may hold none of the tables, and an absence it cannot
// vouch for is no grounds to order every other node to delete what it cannot see.
TEST(router_cluster_test, refuse_to_delete_a_table_this_node_cannot_say_is_missing)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.led_by(cluster::table_key, here, 41);
	router.is_incomplete(true);

	EXPECT_EQ(error_code(router.route(del("/table/account"))), "node_incomplete");
	EXPECT_TRUE(nodes.sent().empty());
}

// A table is a record of no partition, so what orders one is the leader of the tables, and a
// create that lands anywhere else is sent there rather than carried out where it landed.
TEST(router_cluster_test, create_a_table_through_the_node_that_leads_the_tables)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.led_by(cluster::table_key, there, 41);

	router.route(put("/table/account", "{}"));

	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].first, there);
	EXPECT_EQ(nodes.sent()[0].second.term, 0);
	EXPECT_FALSE(repository.has_table("account"));
}

// This node leads the tables, so this is where two creates of one name are decided between — and
// every node is told the term the one that won was decided in.
TEST(router_cluster_test, order_a_table_create_this_node_leads)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.led_by(cluster::table_key, here, 41);

	EXPECT_EQ(router.route(put("/table/account", "{}")).status, boost::beast::http::status::created);
	EXPECT_TRUE(repository.has_table("account"));

	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].first, there);
	EXPECT_EQ(nodes.sent()[0].second.term, 41);
}

// Nothing leading the tables is a create with nowhere to be ordered, and it is refused rather
// than carried out on whichever node happened to take it.
TEST(router_cluster_test, refuse_a_table_create_when_nothing_leads_the_tables)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.led_by_nobody(cluster::table_key);

	router::response response = router.route(put("/table/account", "{}"));

	EXPECT_EQ(error_code(response), "no_leader");
	EXPECT_EQ(response.status, boost::beast::http::status::service_unavailable);
	EXPECT_TRUE(nodes.sent().empty());
	EXPECT_FALSE(repository.has_table("account"));
}

TEST(router_cluster_test, apply_a_table_create_the_leader_ordered)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.led_by(cluster::table_key, there, 60);

	router::request forwarded = put("/table/account", "{}");

	forwarded.forwarded = true;
	forwarded.term = 60;

	EXPECT_EQ(router.route(forwarded).status, boost::beast::http::status::created);
	EXPECT_TRUE(repository.has_table("account"));
	EXPECT_TRUE(nodes.sent().empty());
}

// The same fence a record write has: a leader that lost its lease and does not know it must not
// create a table behind the leader that replaced it.
TEST(router_cluster_test, refuse_a_table_create_ordered_in_a_term_that_has_passed)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.applied(cluster::table_key, 60);

	router::request forwarded = put("/table/account", "{}");

	forwarded.forwarded = true;
	forwarded.term = 41;

	EXPECT_EQ(error_code(router.route(forwarded)), "stale_leader");
	EXPECT_FALSE(repository.has_table("account"));
}

// Two nodes disagreeing about who leads the tables must not bounce a create between them.
TEST(router_cluster_test, refuse_a_table_create_sent_here_to_be_ordered_that_this_node_does_not_lead)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.led_by(cluster::table_key, there, 41);

	router::request forwarded = put("/table/account", "{}");

	forwarded.forwarded = true;

	EXPECT_EQ(error_code(router.route(forwarded)), "no_leader");
	EXPECT_TRUE(nodes.sent().empty());
	EXPECT_FALSE(repository.has_table("account"));
}

// Dropping a table is ordered by the node that orders creating one, so a drop and a create of the
// same name are carried out one after the other rather than at once on two nodes.
TEST(router_cluster_test, delete_a_table_through_the_node_that_leads_the_tables)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.led_by(cluster::table_key, here, 41);

	create_table(router, "account");
	nodes.forget();

	EXPECT_EQ(router.route(del("/table/account")).status, boost::beast::http::status::no_content);
	EXPECT_FALSE(repository.has_table("account"));

	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].second.method, boost::beast::http::verb::delete_);
	EXPECT_EQ(nodes.sent()[0].second.term, 41);
}

TEST(router_cluster_test, delete_a_table_the_leader_ordered_without_passing_it_on)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	nodes.led_by(cluster::table_key, there, 60);

	router::request forwarded = del("/table/account");

	forwarded.forwarded = true;
	forwarded.term = 60;

	EXPECT_EQ(router.route(forwarded).status, boost::beast::http::status::no_content);
	EXPECT_TRUE(nodes.sent().empty());
}

// Every schema operation takes one lock rather than the stripe its name falls in, because what a
// create is valid against is every other table: two of them at once are two nodes disagreeing
// about the graph, and a create validated against a table another node is dropping is the
// dangling edge parse_table exists to refuse.
TEST(router_cluster_test, order_concurrent_table_creates)
{
	repository::fake_repository repository;
	counting_cluster nodes;
	router::router router(repository, nodes);
	std::vector<std::thread> creating;

	nodes.led_by(cluster::table_key, here, 41);

	for (size_t i = 0; i < 4; i++)
	{
		creating.push_back(std::thread(create_table, std::ref(router), "account" + std::to_string(i)));
	}

	for (size_t i = 0; i < creating.size(); i++)
	{
		creating[i].join();
	}

	EXPECT_EQ(nodes.most_at_once(), 1u);
	EXPECT_TRUE(repository.has_table("account3"));
}

TEST(router_cluster_test, scan_every_node_and_answer_in_key_order)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "a", "1");
	write_record(router, "account", "c", "3");
	write_record(router, "account", "e", "5");
	nodes.forget();
	nodes.answer(there, page(boost::json::array { record_json("b", "2"), record_json("d", "4") }, false));

	router::response response = router.route(get("/table/account/key"));

	EXPECT_EQ(keys(response), (std::vector<std::string> { "a", "b", "c", "d", "e" }));
	EXPECT_FALSE(response.json.contains("next"));

	// The bounds of the range travel resolved, because a cursor of this node's is one no other
	// node would take.
	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].second.query, "limit=100&values=true&reverse=false");
}

TEST(router_cluster_test, answer_the_values_of_every_node)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "a", "1");
	nodes.answer(there, page(boost::json::array { record_json("b", "2") }, false));

	boost::json::array records = router.route(get("/table/account/key")).json.at("records").as_array();

	ASSERT_EQ(records.size(), 2u);
	EXPECT_EQ(records[1].as_object().at("value").as_string(), "2");
}

// A record of two parts travels as its halves, so the merge is of the key they compose and not of
// the partition half it arrived under: a key put back short sorts where its partition key does and
// answers as the record sitting there.
TEST(router_cluster_test, a_record_of_two_parts_is_merged_by_the_key_its_halves_compose)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	router.route(put("/table/account/key/4821", "the account"));
	router.route(put("/table/account/key/48210", "another account"));
	nodes.answer(there, page(boost::json::array { record_json("4821", "2019", "a year of it") }, false));

	boost::json::array records = router.route(get("/table/account/key")).json.at("records").as_array();

	ASSERT_EQ(records.size(), 3u);
	EXPECT_EQ(records[1].as_object().at("key").as_string(), "4821");
	EXPECT_EQ(records[1].as_object().at("sort").as_string(), "2019");
	EXPECT_EQ(records[1].as_object().at("value").as_string(), "a year of it");
	EXPECT_EQ(records[2].as_object().at("value").as_string(), "another account");
}

TEST(router_cluster_test, hold_the_merged_page_to_the_limit)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "a", "1");
	write_record(router, "account", "c", "3");
	write_record(router, "account", "e", "5");
	nodes.answer(there, page(boost::json::array { record_json("b", "2"), record_json("d", "4") }, true));

	router::response response = router.route(get("/table/account/key?limit=2"));

	EXPECT_EQ(keys(response), (std::vector<std::string> { "a", "b" }));

	// The cursor is this node's own and names the last key it answered with, so the next page
	// starts with the keys this one dropped.
	EXPECT_EQ(cursor_key(response), "b");
}

// Each node answered within the budget on its own, and the merge of them is the sum: two pages of
// two megabyte records is twelve megabytes of response on the node putting them back in order. So
// the budget is applied again to what they came to, and the cursor names the last key that
// survived it — the same trim the limit gets, for the reason a limit cannot see.
TEST(router_cluster_test, hold_the_merged_page_to_the_budget)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	std::string value(2 * 1024 * 1024, 'v');

	create_table(router, "account");
	write_record(router, "account", "a", value);
	write_record(router, "account", "c", value);
	nodes.answer(there, page(boost::json::array { record_json("b", value), record_json("d", value) }, false));

	router::response response = router.route(get("/table/account/key"));

	EXPECT_EQ(keys(response), (std::vector<std::string> { "a", "b", "c" }));
	EXPECT_EQ(cursor_key(response), "c");
}

TEST(router_cluster_test, page_through_a_scan_of_every_node)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "a", "1");
	write_record(router, "account", "c", "3");
	nodes.answer(there, page(boost::json::array { record_json("b", "2") }, false));

	router::response first = router.route(get("/table/account/key?limit=2"));

	EXPECT_EQ(keys(first), (std::vector<std::string> { "a", "b" }));

	nodes.forget();
	nodes.answer(there, page(boost::json::array(), false));

	std::string cursor = std::string(first.json.at("next").as_string());
	router::response second = router.route(get("/table/account/key?limit=2&cursor=" + cursor));

	EXPECT_EQ(keys(second), (std::vector<std::string> { "c" }));

	// The other node is asked from where the last page ended rather than for a cursor it never
	// issued.
	EXPECT_EQ(nodes.sent()[0].second.query, "limit=2&values=true&reverse=false&from=b%00");
}

TEST(router_cluster_test, scan_every_node_backwards)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "a", "1");
	write_record(router, "account", "c", "3");
	nodes.answer(there, page(boost::json::array { record_json("b", "2"), record_json("d", "4") }, false));

	router::response response = router.route(get("/table/account/key?reverse=true"));

	EXPECT_EQ(keys(response), (std::vector<std::string> { "d", "c", "b", "a" }));
}

TEST(router_cluster_test, scan_a_prefix_of_every_node)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.answer(there, page(boost::json::array(), false));

	router.route(get("/table/account/key?prefix=a"));

	// A prefix is a range, and the other node is asked for the range rather than for the prefix
	// it was written as.
	EXPECT_EQ(nodes.sent()[0].second.query, "limit=100&values=true&reverse=false&from=a&to=b");
}

// A key belongs to one node, so a key from two nodes is a key whose owner changed and the copy
// left behind is passed over rather than answered twice.
TEST(router_cluster_test, answer_a_key_two_nodes_hold_once)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "a", "1");
	nodes.answer(there, page(boost::json::array { record_json("a", "1") }, false));

	EXPECT_EQ(keys(router.route(get("/table/account/key"))), (std::vector<std::string> { "a" }));
}

TEST(router_cluster_test, serve_a_forwarded_scan_where_it_stands)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "a", "1");
	nodes.forget();

	router::request forwarded = get("/table/account/key");

	forwarded.forwarded = true;

	EXPECT_EQ(keys(router.route(forwarded)), (std::vector<std::string> { "a" }));
	EXPECT_TRUE(nodes.sent().empty());
}

TEST(router_cluster_test, fail_to_scan_when_a_node_does_not_answer)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.answer(there, router::error_response("storage_error", "Node \"" + there + "\" did not answer."));

	EXPECT_EQ(error_code(router.route(get("/table/account/key"))), "storage_error");
}

// A zone holds a copy of every key, so a scan is answered by one zone and not by every node: what
// the other zones hold is the same records again.
TEST(router_cluster_test, scan_this_node_s_own_zone_and_no_other)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "a", "1");
	write_record(router, "account", "c", "3");
	nodes.forget();
	nodes.answer(partner, page(boost::json::array { record_json("b", "2") }, false));

	router::response response = router.route(get("/table/account/key"));

	EXPECT_EQ(keys(response), (std::vector<std::string> { "a", "b", "c" }));

	// The partner shares this node's zone, so the page it answers with crosses no zone boundary,
	// and the two zones behind it are not asked at all.
	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].first, partner);
}

// A node that is the only one in its zone holds every key itself, so a scan asks nobody.
TEST(router_cluster_test, scan_nobody_when_this_node_is_a_zone_of_its_own)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "a", "1");
	write_record(router, "account", "b", "2");
	nodes.forget();

	EXPECT_EQ(keys(router.route(get("/table/account/key"))), (std::vector<std::string> { "a", "b" }));
	EXPECT_TRUE(nodes.sent().empty());
}

// The zone behind it holds the same keys, so a node that does not answer is a zone to give up on
// rather than a scan to fail.
TEST(router_cluster_test, scan_the_next_zone_when_a_node_of_the_first_does_not_answer)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "a", "1");
	nodes.forget();
	nodes.answer(partner, router::error_response("storage_error", "Node \"" + partner + "\" did not answer."));
	nodes.answer(there, page(boost::json::array { record_json("b", "2") }, false));
	nodes.answer(elsewhere, page(boost::json::array { record_json("c", "3") }, false));

	router::response response = router.route(get("/table/account/key"));

	EXPECT_EQ(keys(response), (std::vector<std::string> { "a", "b", "c" }));

	ASSERT_EQ(nodes.sent().size(), 3u);
	EXPECT_EQ(nodes.sent()[0].first, partner);
	EXPECT_EQ(nodes.sent()[1].first, there);
	EXPECT_EQ(nodes.sent()[2].first, elsewhere);
}

// A refusal is not a node that is missing: a cursor this instance did not issue is refused by every
// zone alike, so the first one to say so is the answer.
TEST(router_cluster_test, take_a_refusal_of_a_scan_from_the_zone_that_gave_it)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.answer(partner, router::error_response("invalid_cursor", "That cursor is not this instance's."));

	EXPECT_EQ(error_code(router.route(get("/table/account/key"))), "invalid_cursor");
	EXPECT_EQ(nodes.sent().size(), 1u);
}

TEST(router_cluster_test, fail_to_scan_when_no_zone_answers)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.forget();
	nodes.answer(partner, router::error_response("storage_error", "Node \"" + partner + "\" did not answer."));
	nodes.answer(there, router::error_response("storage_error", "Node \"" + there + "\" did not answer."));
	nodes.answer(elsewhere, router::error_response("storage_error", "Node \"" + elsewhere + "\" did not answer."));

	EXPECT_EQ(error_code(router.route(get("/table/account/key"))), "storage_error");

	// Two, not three: a zone is given up on at the first node of it that does not answer, because
	// what the rest of that zone holds is no longer a whole copy of the range.
	EXPECT_EQ(nodes.sent().size(), 2u);
}

TEST(router_cluster_test, delete_a_range_on_every_node)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	write_record(router, "account", "a", "1");
	nodes.forget();

	router::response response = router.route(del("/table/account/key?prefix=a"));

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	EXPECT_TRUE(repository.read_record("account", "a") == std::nullopt);
	ASSERT_EQ(nodes.sent().size(), 1u);
	EXPECT_EQ(nodes.sent()[0].second.method, boost::beast::http::verb::delete_);
	EXPECT_EQ(nodes.sent()[0].second.query, "limit=100&values=true&reverse=false&from=a&to=b");
}

TEST(router_cluster_test, fail_to_delete_a_range_a_node_refuses)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	create_table(router, "account");
	nodes.answer(there, router::error_response("write_stalled", "Writes are stalled."));

	EXPECT_EQ(error_code(router.route(del("/table/account/key?prefix=a"))), "write_stalled");
}

TEST(router_cluster_test, name_the_nodes_of_the_cluster_in_the_health_of_the_instance)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	boost::json::array named = router.route(get("/health")).json.at("nodes").as_array();

	ASSERT_EQ(named.size(), 2u);
	EXPECT_EQ(named[0].as_string(), here);
	EXPECT_EQ(named[1].as_string(), there);
}

// The zones are what say how many copies of a record there are, so they are what an instance says
// about itself.
TEST(router_cluster_test, name_the_zones_of_the_cluster_in_the_health_of_the_instance)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = three_zones();
	router::router router(repository, nodes);

	boost::json::object zones = router.route(get("/health")).json.at("zones").as_object();

	ASSERT_EQ(zones.size(), 3u);
	EXPECT_EQ(zones.at("a").as_array()[0].as_string(), here);
	EXPECT_EQ(zones.at("b").as_array()[0].as_string(), there);
	EXPECT_EQ(zones.at("c").as_array()[0].as_string(), elsewhere);
}

// An election that has not settled is a node leading nothing, and health is where that shows.
TEST(router_cluster_test, count_the_partitions_this_node_leads_in_the_health_of_the_instance)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = paired_zones();
	router::router router(repository, nodes);

	EXPECT_EQ(router.route(get("/health")).json.at("leads").as_int64(), 0);

	nodes.led_by("4821", here, 41);
	nodes.led_by("4822", there, 41);

	EXPECT_EQ(router.route(get("/health")).json.at("leads").as_int64(), 1);
}

TEST(router_cluster_test, name_no_zones_when_the_cluster_has_none)
{
	repository::fake_repository repository;
	cluster::fake_cluster nodes = two_nodes();
	router::router router(repository, nodes);

	router::response response = router.route(get("/health"));

	EXPECT_TRUE(response.json.contains("nodes"));
	EXPECT_FALSE(response.json.contains("zones"));
}

TEST(router_cluster_test, name_no_nodes_when_the_instance_stands_alone)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	EXPECT_FALSE(router.route(get("/health")).json.contains("nodes"));
}

// How a node's share of a table reaches another node. It is served out of this store alone and is
// never forwarded: what is being asked for is what this node holds.
TEST(router_test, answers_a_file_of_the_records_of_the_partitions_asked_for)
{
	repository::fake_repository repository;
	repository::fake_repository taking;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "1", "one");
	write_record(router, "account", "2", "two");

	taking.create_table(table::valid_table("account", std::vector<std::string>()));

	router::response response = router.route(get("/table/account/file?partitions=" + only("1")));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.content_type, router::file_content_type);
	EXPECT_EQ(1u, response.file.records);
	EXPECT_TRUE(response.file.next.empty());

	EXPECT_EQ(1u, taking.import_records("account", response.text));
	EXPECT_TRUE(taking.read_record("account", "1").has_value());
	EXPECT_FALSE(taking.read_record("account", "2").has_value());
}

TEST(router_test, answers_no_file_of_a_table_that_is_not_there)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	router::response response = router.route(get("/table/account/file?partitions=" + every_partition()));

	EXPECT_EQ(response.status, boost::beast::http::status::not_found);
	EXPECT_EQ(error_code(response), "table_not_found");
}

// A set this cluster does not agree with is a node that cuts the keyspace up some other way, and
// answering it would be answering a share nobody asked for.
TEST(router_test, refuses_a_file_of_something_that_is_not_a_set_of_partitions)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	EXPECT_EQ(error_code(router.route(get("/table/account/file"))), "invalid_partitions");
	EXPECT_EQ(error_code(router.route(get("/table/account/file?partitions=ff"))), "invalid_partitions");
}

TEST(router_test, refuses_a_file_resumed_at_something_that_is_not_a_cursor)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	router::response response = router.route(
		get("/table/account/file?partitions=" + every_partition() + "&from=not%20base64"));

	EXPECT_EQ(error_code(response), "invalid_cursor");
}

TEST(router_test, a_file_is_read_and_never_written)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	router::response response = router.route(del("/table/account/file?partitions=" + every_partition()));

	EXPECT_EQ(error_code(response), "method_not_allowed");
}

// What a node clearing down asks the node that owns its share: which of these keys have you got.
// The answer carries no values, because what is being decided is where a record belongs.
TEST(router_test, answers_a_file_of_keys_alone_when_the_values_are_not_wanted)
{
	repository::fake_repository repository;
	repository::fake_repository giving;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "1", "one");

	giving.create_table(table::valid_table("account", std::vector<std::string>()));
	giving.write_record("account", record::valid_record("1", "mine"));
	giving.write_record("account", record::valid_record("2", "mine"));

	router::response response =
		router.route(get("/table/account/file?values=false&partitions=" + every_partition()));

	EXPECT_EQ(1u, response.file.records);

	// The key the owner answered with is the copy this store may give up, and no other.
	EXPECT_EQ(1u, giving.clear_records("account", response.text));
	EXPECT_FALSE(giving.read_record("account", "1").has_value());
	EXPECT_TRUE(giving.read_record("account", "2").has_value());
}

// Where this node would cut a walk of its own table up, so that a node reading it can ask for
// several pieces of it at once.
TEST(router_test, says_where_a_table_would_be_cut_up)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	for (size_t i = 0; i < 8; i++)
	{
		write_record(router, "account", std::to_string(10 + i), "a value");
	}

	router::response response = router.route(get("/table/account/split?ways=4"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);

	const boost::json::array &keys = response.json.at("keys").as_array();

	// One fewer key than the ways asked for, because the last piece runs to the end of the table.
	ASSERT_EQ(3u, keys.size());

	for (size_t i = 0; i < keys.size(); i++)
	{
		EXPECT_TRUE(base64::decode(std::string(keys[i].as_string())).has_value());
	}
}

TEST(router_test, cuts_a_table_up_no_ways_when_it_is_asked_for_one_piece)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "1", "one");

	EXPECT_TRUE(router.route(get("/table/account/split?ways=1")).json.at("keys").as_array().empty());
}

TEST(router_test, refuses_a_split_of_a_table_that_is_not_there)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	EXPECT_EQ(error_code(router.route(get("/table/account/split?ways=4"))), "table_not_found");
}

TEST(router_test, refuses_a_split_into_something_that_is_not_a_number_of_ways)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	EXPECT_EQ(error_code(router.route(get("/table/account/split"))), "invalid_range");
	EXPECT_EQ(error_code(router.route(get("/table/account/split?ways=lots"))), "invalid_range");
	EXPECT_EQ(error_code(router.route(get("/table/account/split?ways=0"))), "invalid_range");
}

// The ends of a piece, which are what make the pieces of a split walk a cover: `to` is the last key
// in the piece and `from` is the key the next one starts after.
TEST(router_test, answers_a_file_that_ends_where_it_was_told_to)
{
	repository::fake_repository repository;
	repository::fake_repository taking;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "1", "one");
	write_record(router, "account", "2", "two");
	write_record(router, "account", "3", "three");

	taking.create_table(table::valid_table("account", std::vector<std::string>()));

	router::response response = router.route(
		get("/table/account/file?partitions=" + every_partition() + "&to=" + url::encode(base64::encode("2"))));

	EXPECT_EQ(2u, response.file.records);
	EXPECT_EQ(2u, taking.import_records("account", response.text));
	EXPECT_FALSE(taking.read_record("account", "3").has_value());
}

// How much of the table one file walks is the asking node's to say, because it is the asking node
// that holds the file.
TEST(router_test, answers_a_file_of_no_more_of_the_table_than_it_was_asked_for)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "1", "one");
	write_record(router, "account", "2", "two");

	router::response response =
		router.route(get("/table/account/file?bytes=1&partitions=" + every_partition()));

	EXPECT_EQ(1u, response.file.records);
	EXPECT_FALSE(response.file.next.empty());
}

TEST(router_test, write_then_read_a_record_of_two_parts)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");

	router::response written = router.route(put("/table/account/key/4821/2019", "Eleanor Whitmore"));

	EXPECT_EQ(written.status, boost::beast::http::status::no_content);
	EXPECT_EQ(router.route(get("/table/account/key/4821/2019")).text, "Eleanor Whitmore");
}

// The sort key is part of what a record is, so a partition key of its own is a key of its own.
TEST(router_test, a_partition_key_and_a_key_sorting_under_it_are_different_records)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	router.route(put("/table/account/key/4821", "the account"));
	router.route(put("/table/account/key/4821/2019", "a year of it"));

	EXPECT_EQ(router.route(get("/table/account/key/4821")).text, "the account");
	EXPECT_EQ(router.route(get("/table/account/key/4821/2019")).text, "a year of it");
}

TEST(router_test, delete_a_record_of_two_parts)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	router.route(put("/table/account/key/4821/2019", "Eleanor Whitmore"));

	EXPECT_EQ(router.route(del("/table/account/key/4821/2019")).status, boost::beast::http::status::no_content);
	EXPECT_EQ(router.route(get("/table/account/key/4821/2019")).status, boost::beast::http::status::not_found);
	EXPECT_EQ(router.route(get("/table/account/key/4821")).status, boost::beast::http::status::not_found);
}

// The two halves are one key with a zero byte between them, so the two ways of writing it down
// name one record.
TEST(router_test, a_key_of_two_parts_is_the_key_carrying_a_zero_byte)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	router.route(put("/table/account/key/4821/2019", "Eleanor Whitmore"));

	EXPECT_EQ(router.route(get("/table/account/key/4821%002019")).text, "Eleanor Whitmore");
}

TEST(router_test, a_scan_says_which_half_of_a_key_is_which)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	router.route(put("/table/account/key/4821/2019", "a year of it"));

	boost::json::object record =
		router.route(get("/table/account/key")).json.at("records").as_array()[0].as_object();

	EXPECT_EQ(record.at("key"), "4821");
	EXPECT_EQ(record.at("sort"), "2019");
	EXPECT_EQ(record.at("value"), "a year of it");
}

TEST(router_test, a_scan_of_keys_of_one_part_says_nothing_about_sorting)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	write_record(router, "account", "4821", "Eleanor Whitmore");

	boost::json::object record =
		router.route(get("/table/account/key")).json.at("records").as_array()[0].as_object();

	EXPECT_EQ(record.at("key"), "4821");
	EXPECT_FALSE(record.contains("sort"));
}

// What the separator buys a scan: a partition key's records are together and in sort key order,
// and they are before every key the partition key is a prefix of.
TEST(router_test, records_of_one_partition_key_scan_together_in_sort_key_order)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	router.route(put("/table/account/key/48210", "another account"));
	router.route(put("/table/account/key/4821/2020", "a later year"));
	router.route(put("/table/account/key/4821/2019", "a year"));
	router.route(put("/table/account/key/4821", "the account"));

	std::vector<std::string> values;
	boost::json::array records = router.route(get("/table/account/key")).json.at("records").as_array();

	for (size_t i = 0; i < records.size(); i++)
	{
		values.push_back(std::string(records[i].as_object().at("value").as_string()));
	}

	EXPECT_EQ(values, (std::vector<std::string> { "the account", "a year", "a later year", "another account" }));
}

// One partition key and nothing else is a bounded range, because a prefix is a prefix of the
// bytes: it cannot tell the separator from a key that carries those bytes and more.
TEST(router_test, a_partition_key_and_nothing_else_is_a_bounded_range)
{
	repository::fake_repository repository;
	cluster::fake_cluster alone = lone_node();
	router::router router(repository, alone);

	create_table(router, "account");
	router.route(put("/table/account/key/4821", "the account"));
	router.route(put("/table/account/key/4821/2019", "a year"));
	router.route(put("/table/account/key/48210", "another account"));

	EXPECT_EQ(keys(router.route(get("/table/account/key?from=4821&to=4821%01"))).size(), 2u);
	EXPECT_EQ(keys(router.route(get("/table/account/key?prefix=4821"))).size(), 3u);
}
