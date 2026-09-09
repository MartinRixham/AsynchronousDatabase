#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <boost/beast/http.hpp>

#include "cluster/forwarder.h"
#include "http/fake_http_client.h"

namespace
{
	const std::string one = "http://asyncdb-1:8080";

	const std::string two = "http://asyncdb-2:8080";

	router::request request(boost::beast::http::verb method, const std::vector<std::string> &path)
	{
		return { method, path, "", "", false };
	}

	std::string error_code(const router::response &response)
	{
		return std::string(response.json.at("error").as_object().at("code").as_string());
	}

	std::string error_message(const router::response &response)
	{
		return std::string(response.json.at("error").as_object().at("message").as_string());
	}
}

TEST(forwarder_test, forward_a_request_to_another_node)
{
	http::fake_client http;

	http.answer(one, http::answer(200, "text/plain; charset=utf-8", "a value"));

	cluster::forwarder forwarder(http);
	router::response response =
		forwarder.forward(one, request(boost::beast::http::verb::get, { "table", "account", "key", "4821" }));

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.text, "a value");

	ASSERT_EQ(http.sent_to(one).size(), 1u);

	http::request sent = http.sent_to(one)[0];

	EXPECT_EQ(sent.method, "GET");
	EXPECT_EQ(sent.url, one + "/table/account/key/4821");
	ASSERT_EQ(sent.headers.size(), 1u);
	EXPECT_EQ(sent.headers[0], "X-Asyncdb-Forwarded: true");
}

TEST(forwarder_test, forward_a_key_that_holds_punctuation_of_a_url)
{
	http::fake_client http;

	http.answer(one, http::answer(200, "text/plain; charset=utf-8", "a value"));

	cluster::forwarder forwarder(http);

	forwarder.forward(one, request(boost::beast::http::verb::get, { "table", "account", "key", "a/b?c" }));

	EXPECT_EQ(http.sent()[0].url, one + "/table/account/key/a%2Fb%3Fc");
}

// A node is asked for what it holds and not for its root, so a request naming no path at all is
// still a request of the node rather than of nothing.
TEST(forwarder_test, forward_a_request_that_names_no_path)
{
	http::fake_client http;

	http.answer(one, http::answer(200, "application/json", "{}"));

	cluster::forwarder forwarder(http);

	forwarder.forward(one, request(boost::beast::http::verb::get, {}));

	EXPECT_EQ(http.sent()[0].url, one + "/");
}

TEST(forwarder_test, forward_a_query_as_it_stands)
{
	http::fake_client http;

	http.answer(one, http::answer(200, "application/json", "{\"records\":[]}"));

	cluster::forwarder forwarder(http);
	router::request scan { boost::beast::http::verb::get, { "table", "account", "key" }, "limit=10&from=a", "", false };

	forwarder.forward(one, scan);

	EXPECT_EQ(http.sent()[0].url, one + "/table/account/key?limit=10&from=a");
}

TEST(forwarder_test, forward_a_body)
{
	http::fake_client http;

	http.answer(one, http::answer(204, "", ""));

	cluster::forwarder forwarder(http);
	router::request write {
		boost::beast::http::verb::put,
		{ "table", "account", "key", "4821" },
		"",
		"a value",
		false
	};

	router::response response = forwarder.forward(one, write);

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	EXPECT_TRUE(response.content_type.empty());
	EXPECT_EQ(http.sent()[0].method, "PUT");
	EXPECT_EQ(http.sent()[0].body, "a value");
}

// A write from the leader carries the term it was ordered in, so that a copy can refuse one older
// than the newest it has applied.
TEST(forwarder_test, forward_the_term_a_write_was_ordered_in)
{
	http::fake_client http;

	http.answer(one, http::answer(204, "", ""));

	cluster::forwarder forwarder(http);
	router::request write {
		boost::beast::http::verb::put,
		{ "table", "account", "key", "4821" },
		"",
		"a value",
		false,
		60
	};

	forwarder.forward(one, write);

	ASSERT_EQ(http.sent()[0].headers.size(), 2u);
	EXPECT_EQ(http.sent()[0].headers[1], "X-Asyncdb-Term: 60");
}

// A write to the leader carries no term, which is what tells the two hops of a write apart.
TEST(forwarder_test, forward_no_term_when_the_request_was_ordered_in_none)
{
	http::fake_client http;

	http.answer(one, http::answer(204, "", ""));

	cluster::forwarder forwarder(http);

	forwarder.forward(one, request(boost::beast::http::verb::put, { "table", "account", "key", "4821" }));

	EXPECT_EQ(http.sent()[0].headers.size(), 1u);
}

// The node that owns the key is the only one that can say how large the value is, and it says so
// without sending it: what asking costs is the headers of the value and not the value.
TEST(forwarder_test, forward_a_head_request_as_a_head)
{
	http::fake_client http;

	http.answer(one, http::head_answer(200, "text/plain; charset=utf-8", 7));

	cluster::forwarder forwarder(http);
	router::response response =
		forwarder.forward(one, request(boost::beast::http::verb::head, { "table", "account", "key", "4821" }));

	EXPECT_EQ(http.sent()[0].method, "HEAD");
	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.content_type, "text/plain; charset=utf-8");
	EXPECT_EQ(response.length, 7u);

	// The length of a body that is not there is not a body to answer with.
	EXPECT_TRUE(router::response_body(response).empty());
}

// A key nothing holds is answered the same way a local miss is, so a HEAD of one is a 404 with
// nothing to say about a length.
TEST(forwarder_test, answer_a_head_of_a_key_that_is_not_there)
{
	http::fake_client http;

	http.answer(one, http::head_answer(404, "", 0));

	cluster::forwarder forwarder(http);
	router::response response =
		forwarder.forward(one, request(boost::beast::http::verb::head, { "table", "account", "key", "4821" }));

	EXPECT_EQ(response.status, boost::beast::http::status::not_found);
	EXPECT_EQ(response.length, 0u);
}

TEST(forwarder_test, answer_a_document_as_a_document)
{
	http::fake_client http;

	http.answer(one, http::answer(404, "application/json", "{\"error\":{\"code\":\"table_not_found\"}}"));

	cluster::forwarder forwarder(http);
	router::response response =
		forwarder.forward(one, request(boost::beast::http::verb::get, { "table", "account", "key", "4821" }));

	EXPECT_EQ(response.status, boost::beast::http::status::not_found);
	EXPECT_EQ(error_code(response), "table_not_found");
}

TEST(forwarder_test, answer_a_storage_error_when_the_node_is_not_there)
{
	http::fake_client http;
	cluster::forwarder forwarder(http);
	router::response response =
		forwarder.forward(one, request(boost::beast::http::verb::get, { "table", "account", "key", "4821" }));

	EXPECT_EQ(response.status, boost::beast::http::status::internal_server_error);
	EXPECT_EQ(error_code(response), "storage_error");
	EXPECT_NE(error_message(response).find(one), std::string::npos);
}

TEST(forwarder_test, answer_a_storage_error_when_the_node_answers_with_something_else)
{
	http::fake_client http;

	http.answer(one, http::answer(200, "application/json", "not a document"));

	cluster::forwarder forwarder(http);
	router::response response =
		forwarder.forward(one, request(boost::beast::http::verb::get, { "table", "account", "key", "4821" }));

	EXPECT_EQ(response.status, boost::beast::http::status::internal_server_error);
	EXPECT_EQ(error_code(response), "storage_error");
}

TEST(forwarder_test, forward_one_request_to_every_node_named)
{
	http::fake_client http;

	http.answer(one, http::answer(204, "", ""));
	http.answer(two, http::answer(204, "", ""));

	cluster::forwarder forwarder(http);
	router::request write {
		boost::beast::http::verb::put,
		{ "table", "account", "key", "4821" },
		"",
		"a value",
		false,
		60
	};

	std::vector<router::response> responses = forwarder.forward_all({ one, two }, write);

	ASSERT_EQ(responses.size(), 2u);
	EXPECT_EQ(responses[0].status, boost::beast::http::status::no_content);
	EXPECT_EQ(responses[1].status, boost::beast::http::status::no_content);

	ASSERT_EQ(http.sent().size(), 2u);
	EXPECT_EQ(http.sent()[0].url, one + "/table/account/key/4821");
	EXPECT_EQ(http.sent()[1].url, two + "/table/account/key/4821");
	EXPECT_EQ(http.sent()[0].body, "a value");
	EXPECT_EQ(http.sent()[1].body, "a value");
	EXPECT_EQ(http.sent()[1].headers[1], "X-Asyncdb-Term: 60");
}

// The answers come back in the order the nodes were named and not the order they answered in,
// which is what lets the caller tell whose answer is whose.
TEST(forwarder_test, answer_in_the_order_the_nodes_were_named)
{
	http::fake_client http;

	http.answer(two, http::answer(409, "application/json", "{\"error\":{\"code\":\"stale_leader\"}}"));
	http.answer(one, http::answer(204, "", ""));

	cluster::forwarder forwarder(http);
	std::vector<router::response> responses =
		forwarder.forward_all({ one, two }, request(boost::beast::http::verb::put, { "table", "account" }));

	ASSERT_EQ(responses.size(), 2u);
	EXPECT_EQ(responses[0].status, boost::beast::http::status::no_content);
	EXPECT_EQ(error_code(responses[1]), "stale_leader");
}

// A node that did not answer is an answer of its own, reported against the node it belongs to.
TEST(forwarder_test, answer_for_a_node_of_a_fan_out_that_is_not_there)
{
	http::fake_client http;

	http.answer(one, http::answer(204, "", ""));

	cluster::forwarder forwarder(http);
	std::vector<router::response> responses =
		forwarder.forward_all({ one, two }, request(boost::beast::http::verb::put, { "table", "account" }));

	ASSERT_EQ(responses.size(), 2u);
	EXPECT_EQ(responses[0].status, boost::beast::http::status::no_content);
	EXPECT_EQ(error_code(responses[1]), "storage_error");
	EXPECT_NE(error_message(responses[1]).find(two), std::string::npos);
}

TEST(forwarder_test, ask_nobody_when_no_node_is_named)
{
	http::fake_client http;
	cluster::forwarder forwarder(http);

	EXPECT_TRUE(forwarder.forward_all({}, request(boost::beast::http::verb::put, { "table", "account" })).empty());
	EXPECT_TRUE(http.sent().empty());
}

// A file of records answers with two things its bytes cannot say, and they are headers rather than
// fields of a document. What they are here is what the node that asked reads them back as.
TEST(forwarder_test, reads_back_what_a_file_of_records_said_beside_its_bytes)
{
	http::fake_client http;
	http::response answered = http::answer(200, router::file_content_type, "the file");

	answered.headers.push_back(std::pair<std::string, std::string>(router::records_header, "17"));
	answered.headers.push_back(std::pair<std::string, std::string>(router::next_header, "YSBrZXk="));

	http.answer(one, answered);

	cluster::forwarder forwarder(http);
	router::response response =
		forwarder.forward(one, request(boost::beast::http::verb::get, { "table", "account", "file" }));

	EXPECT_EQ(response.text, "the file");
	EXPECT_EQ(17u, response.file.records);
	EXPECT_EQ("YSBrZXk=", response.file.next);
}

// A node that answered something else says nothing here, so a walk reading an answer that is not a
// file is a walk with nowhere to resume rather than one that resumes at nothing.
TEST(forwarder_test, an_answer_that_is_not_a_file_carries_no_transfer)
{
	http::fake_client http;

	http.answer(one, http::answer(200, "text/plain; charset=utf-8", "a value"));

	cluster::forwarder forwarder(http);
	router::response response =
		forwarder.forward(one, request(boost::beast::http::verb::get, { "table", "account", "key", "4821" }));

	EXPECT_EQ(0u, response.file.records);
	EXPECT_TRUE(response.file.next.empty());
}

// The fan out for a caller asking each node something different, which is what reading a share in
// several pieces at once is. Every node gets its own request, and the answers come back in the
// order the enquiries were given rather than the order the nodes answered in.
TEST(forwarder_test, forward_a_different_request_to_each_node)
{
	http::fake_client http;

	http.answer(one, http::answer(200, "text/plain; charset=utf-8", "from one"));
	http.answer(two, http::answer(200, "text/plain; charset=utf-8", "from two"));

	cluster::forwarder forwarder(http);

	std::vector<cluster::enquiry> enquiries {
		cluster::enquiry { one, request(boost::beast::http::verb::get, { "table", "account", "file" }) },
		cluster::enquiry { two, request(boost::beast::http::verb::get, { "table", "summary", "file" }) }
	};

	std::vector<router::response> responses = forwarder.forward_each(enquiries);

	ASSERT_EQ(2u, responses.size());
	EXPECT_EQ("from one", responses[0].text);
	EXPECT_EQ("from two", responses[1].text);

	ASSERT_EQ(1u, http.sent_to(one).size());
	ASSERT_EQ(1u, http.sent_to(two).size());
	EXPECT_NE(std::string::npos, http.sent_to(one)[0].url.find("/table/account/file"));
	EXPECT_NE(std::string::npos, http.sent_to(two)[0].url.find("/table/summary/file"));
}

TEST(forwarder_test, forward_nothing_to_nobody)
{
	http::fake_client http;
	cluster::forwarder forwarder(http);

	EXPECT_TRUE(forwarder.forward_each(std::vector<cluster::enquiry>()).empty());
	EXPECT_TRUE(http.sent().empty());
}
