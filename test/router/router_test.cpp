#include <string>

#include <gtest/gtest.h>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#include "base64/base64.h"
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
	router::router router(repository);

	router::response response = router.route(get("/wibble"));

	EXPECT_EQ(response.status, boost::beast::http::status::not_found);
	EXPECT_EQ(error_code(response), "not_found");
}

TEST(router_test, health_says_whether_writes_are_stalled)
{
	repository::fake_repository repository;
	router::router router(repository);

	router::response response = router.route(get("/health"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.json.at("status"), "ok");
	EXPECT_EQ(response.json.at("write_stalled"), false);

	repository.stall();

	EXPECT_EQ(router.route(get("/health")).json.at("write_stalled"), true);
}

TEST(router_test, list_no_tables)
{
	repository::fake_repository repository;
	router::router router(repository);

	router::response response = router.route(get("/table"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.json.at("tables").as_array().size(), 0);
}

TEST(router_test, create_a_table)
{
	repository::fake_repository repository;
	router::router router(repository);

	router::response response = router.route(put("/table/account", "{}"));

	EXPECT_EQ(response.status, boost::beast::http::status::created);
	EXPECT_EQ(response.json.at("name"), "account");
	EXPECT_TRUE(repository.has_table("account"));
}

TEST(router_test, create_a_table_with_no_body_at_all)
{
	repository::fake_repository repository;
	router::router router(repository);

	router::response response = router.route(put("/table/account", ""));

	EXPECT_EQ(response.status, boost::beast::http::status::created);
	EXPECT_EQ(response.json.at("dependencies").as_array().size(), 0);
}

TEST(router_test, creating_the_same_table_again_changes_nothing)
{
	repository::fake_repository repository;
	router::router router(repository);

	router.route(put("/table/account", "{}"));

	router::response response = router.route(put("/table/account", "{\"dependencies\":[]}"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.json.at("name"), "account");
}

TEST(router_test, fail_to_create_a_table_that_exists_with_different_options)
{
	repository::fake_repository repository;
	router::router router(repository);

	create_table(router, "account");

	router::response response = router.route(put("/table/account", "{\"dependencies\":[\"account\"]}"));

	EXPECT_EQ(response.status, boost::beast::http::status::conflict);
	EXPECT_EQ(error_code(response), "table_exists");
}

TEST(router_test, fail_to_create_a_table_with_an_invalid_name)
{
	repository::fake_repository repository;
	router::router router(repository);

	router::response response = router.route(put("/table/An%2FAccount", "{}"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_table_name");
	EXPECT_FALSE(repository.has_table("An Account"));
}

TEST(router_test, fail_to_create_a_table_from_a_body_that_is_not_json)
{
	repository::fake_repository repository;
	router::router router(repository);

	router::response response = router.route(put("/table/account", "not json"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_body");
	EXPECT_FALSE(repository.has_table("account"));
}

TEST(router_test, fail_to_create_a_table_from_a_body_that_is_not_an_object)
{
	repository::fake_repository repository;
	router::router router(repository);

	router::response response = router.route(put("/table/account", "[]"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_body");
	EXPECT_FALSE(repository.has_table("account"));
}

TEST(router_test, fail_to_create_a_table_that_depends_on_one_that_is_not_there)
{
	repository::fake_repository repository;
	router::router router(repository);

	router::response response = router.route(put("/table/transaction", "{\"dependencies\":[\"account\"]}"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "dependency_not_found");
	EXPECT_FALSE(repository.has_table("transaction"));
}

TEST(router_test, list_the_tables_and_their_dependencies)
{
	repository::fake_repository repository;
	router::router router(repository);

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
	router::router router(repository);

	create_table(router, "account");

	router::response response = router.route(get("/table/account"));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.json.at("name"), "account");
	EXPECT_EQ(response.json.at("dependencies").as_array().size(), 0);
}

TEST(router_test, fail_to_inspect_a_table_that_is_not_there)
{
	repository::fake_repository repository;
	router::router router(repository);

	router::response response = router.route(get("/table/account"));

	EXPECT_EQ(response.status, boost::beast::http::status::not_found);
	EXPECT_EQ(error_code(response), "table_not_found");
}

TEST(router_test, delete_a_table_and_its_data)
{
	repository::fake_repository repository;
	router::router router(repository);

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
	router::router router(repository);

	router::response response = router.route(del("/table/account"));

	EXPECT_EQ(response.status, boost::beast::http::status::not_found);
	EXPECT_EQ(error_code(response), "table_not_found");
}

TEST(router_test, write_then_read_a_record)
{
	repository::fake_repository repository;
	router::router router(repository);

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
	router::router router(repository);

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
	router::router router(repository);

	create_table(router, "account");
	write_record(router, "account", "4821", "{\"firstName\":\"Eleanor\"");

	EXPECT_EQ(router.route(get("/table/account/key/4821")).text, "{\"firstName\":\"Eleanor\"");
}

TEST(router_test, delete_a_record)
{
	repository::fake_repository repository;
	router::router router(repository);

	create_table(router, "account");
	write_record(router, "account", "4821", "Eleanor Whitmore");

	router::response response = router.route(del("/table/account/key/4821"));

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	EXPECT_EQ(router.route(get("/table/account/key/4821")).status, boost::beast::http::status::not_found);
}

TEST(router_test, deleting_a_record_that_is_not_there_is_a_no_op)
{
	repository::fake_repository repository;
	router::router router(repository);

	create_table(router, "account");

	EXPECT_EQ(router.route(del("/table/account/key/4821")).status, boost::beast::http::status::no_content);
}

TEST(router_test, fail_to_read_a_record_of_a_table_that_is_not_there)
{
	repository::fake_repository repository;
	router::router router(repository);

	router::response response = router.route(get("/table/account/key/4821"));

	EXPECT_EQ(response.status, boost::beast::http::status::not_found);
	EXPECT_EQ(error_code(response), "table_not_found");
}

TEST(router_test, fail_to_read_a_key_that_is_not_valid_utf8)
{
	repository::fake_repository repository;
	router::router router(repository);

	create_table(router, "account");

	router::response response = router.route(get("/table/account/key/%C3%28"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_key_encoding");
}

TEST(router_test, fail_to_write_a_key_that_is_too_large)
{
	repository::fake_repository repository;
	router::router router(repository);

	create_table(router, "account");

	router::response response =
		router.route(put("/table/account/key/" + std::string(record::max_key_size + 1, 'k'), "a value"));

	EXPECT_EQ(response.status, boost::beast::http::status::payload_too_large);
	EXPECT_EQ(error_code(response), "key_too_large");
}

TEST(router_test, fail_to_write_a_value_that_is_too_large)
{
	repository::fake_repository repository;
	router::router router(repository);

	create_table(router, "account");

	router::response response =
		router.route(put("/table/account/key/4821", std::string(record::max_value_size + 1, 'v')));

	EXPECT_EQ(response.status, boost::beast::http::status::payload_too_large);
	EXPECT_EQ(error_code(response), "value_too_large");
}

TEST(router_test, scan_a_table_in_key_order)
{
	repository::fake_repository repository;
	router::router router(repository);

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
	router::router router(repository);

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
	router::router router(repository);

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
	router::router router(repository);

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
	router::router router(repository);

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
	router::router router(repository);

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

TEST(router_test, fail_to_scan_with_a_cursor_this_instance_did_not_issue)
{
	repository::fake_repository repository;
	router::router router(repository);

	create_table(router, "account");

	router::response response =
		router.route(get("/table/account/key?cursor=" + scan::encode_cursor("1", "another instance")));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_cursor");
}

TEST(router_test, fail_to_scan_a_range_that_is_not_below_its_end)
{
	repository::fake_repository repository;
	router::router router(repository);

	create_table(router, "account");

	router::response response = router.route(get("/table/account/key?from=b&to=a"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_range");
}

TEST(router_test, fail_to_scan_a_table_that_is_not_there)
{
	repository::fake_repository repository;
	router::router router(repository);

	EXPECT_EQ(error_code(router.route(get("/table/account/key"))), "table_not_found");
}

TEST(router_test, delete_a_range)
{
	repository::fake_repository repository;
	router::router router(repository);

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
	router::router router(repository);

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
	router::router router(repository);

	create_table(router, "account");

	router::response response = router.route(request(boost::beast::http::verb::post, "/table/account", "{}"));

	EXPECT_EQ(response.status, boost::beast::http::status::method_not_allowed);
	EXPECT_EQ(error_code(response), "method_not_allowed");
}

TEST(router_test, a_path_below_a_key_is_not_a_route)
{
	repository::fake_repository repository;
	router::router router(repository);

	create_table(router, "account");

	EXPECT_EQ(router.route(get("/table/account/key/4821/name")).status, boost::beast::http::status::not_found);
	EXPECT_EQ(router.route(get("/table/account/wibble")).status, boost::beast::http::status::not_found);
}

namespace
{
	const std::string here = "http://asyncdb-1:8080";

	const std::string there = "http://asyncdb-2:8080";

	const std::string elsewhere = "http://asyncdb-3:8080";

	cluster::fake_cluster two_nodes()
	{
		return cluster::fake_cluster(here, { here, there });
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

	std::string cursor_key(const router::response &response)
	{
		std::string decoded;

		base64::decode(std::string(response.json.at("next").as_string()), &decoded);

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

	// The copy that refused is the last one asked: the request is not sent on to the zones behind
	// it once the client is going to be told to write it again.
	EXPECT_EQ(nodes.sent().size(), 1u);
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
	router::router router(repository);

	EXPECT_FALSE(router.route(get("/health")).json.contains("nodes"));
}
