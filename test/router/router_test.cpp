#include <string>

#include <gtest/gtest.h>
#include <boost/beast.hpp>
#include <boost/json.hpp>

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

	router::response response = router.route(put("/table/An%20Account", "{}"));

	EXPECT_EQ(response.status, boost::beast::http::status::bad_request);
	EXPECT_EQ(error_code(response), "invalid_table_name");
	EXPECT_FALSE(repository.has_table("An Account"));
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
