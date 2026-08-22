#include <string>

#include <gtest/gtest.h>
#include <boost/json.hpp>

#include "base64/base64.h"
#include "etcd/etcd_client.h"
#include "http/fake_http_client.h"

namespace
{
	boost::json::object body_of(const http::request &request)
	{
		return boost::json::parse(request.body).as_object();
	}

	std::string decoded(const boost::json::object &json, const std::string &field)
	{
		std::string text;

		base64::decode(std::string(json.at(field).as_string()), &text);

		return text;
	}
}

TEST(etcd_client_test, grant_a_lease)
{
	http::fake_client http;

	http.answer("/v3/lease/grant", http::answer(200, "application/json", "{\"ID\":\"7587840301820862481\",\"TTL\":\"10\"}"));

	etcd::client client(http, { "http://etcd:2379" });
	std::optional<int64_t> lease = client.grant_lease(10);

	ASSERT_TRUE(lease.has_value());
	EXPECT_EQ(*lease, 7587840301820862481);
	ASSERT_EQ(http.sent().size(), 1u);
	EXPECT_EQ(http.sent()[0].method, "POST");
	EXPECT_EQ(http.sent()[0].url, "http://etcd:2379/v3/lease/grant");

	// Every 64 bit number in the gateway's JSON is a string, because a lease id does not survive
	// a JSON number.
	EXPECT_EQ(body_of(http.sent()[0]).at("TTL").as_string(), "10");
}

TEST(etcd_client_test, fail_to_grant_a_lease_when_etcd_does_not_answer)
{
	http::fake_client http;
	etcd::client client(http, { "http://etcd:2379" });

	EXPECT_FALSE(client.grant_lease(10).has_value());
}

TEST(etcd_client_test, fail_to_grant_a_lease_when_etcd_answers_with_an_error)
{
	http::fake_client http;

	http.answer("/v3/lease/grant", http::answer(500, "application/json", "{\"error\":\"no leader\"}"));

	etcd::client client(http, { "http://etcd:2379" });

	EXPECT_FALSE(client.grant_lease(10).has_value());
}

TEST(etcd_client_test, keep_a_lease_alive)
{
	http::fake_client http;

	http.answer(
		"/v3/lease/keepalive",
		http::answer(200, "application/json", "{\"result\":{\"ID\":\"12\",\"TTL\":\"10\"}}"));

	etcd::client client(http, { "http://etcd:2379" });

	EXPECT_TRUE(client.keep_alive(12));
	EXPECT_EQ(body_of(http.sent()[0]).at("ID").as_string(), "12");
}

// A lease that has run out is renewed to nothing rather than refused, so the answer has to be read
// rather than counted: a node whose lease is gone registers again.
TEST(etcd_client_test, fail_to_keep_a_lease_that_has_expired)
{
	http::fake_client http;

	http.answer("/v3/lease/keepalive", http::answer(200, "application/json", "{\"result\":{\"ID\":\"12\",\"TTL\":\"0\"}}"));

	etcd::client client(http, { "http://etcd:2379" });

	EXPECT_FALSE(client.keep_alive(12));
}

TEST(etcd_client_test, put_a_key)
{
	http::fake_client http;

	http.answer("/v3/kv/put", http::answer(200, "application/json", "{\"header\":{}}"));

	etcd::client client(http, { "http://etcd:2379" });

	EXPECT_TRUE(client.put("/asyncdb/node/one", "http://one:8080", 12));

	boost::json::object body = body_of(http.sent()[0]);

	EXPECT_EQ(decoded(body, "key"), "/asyncdb/node/one");
	EXPECT_EQ(decoded(body, "value"), "http://one:8080");
	EXPECT_EQ(body.at("lease").as_string(), "12");
}

TEST(etcd_client_test, read_a_range_of_keys)
{
	http::fake_client http;
	boost::json::object one {
		{ "key", base64::encode("/asyncdb/node/http://one:8080") },
		{ "value", base64::encode("http://one:8080") }
	};

	boost::json::object two {
		{ "key", base64::encode("/asyncdb/node/http://two:8080") },
		{ "value", base64::encode("http://two:8080") }
	};

	boost::json::object answer { { "kvs", boost::json::array { one, two } } };

	http.answer("/v3/kv/range", http::answer(200, "application/json", boost::json::serialize(answer)));

	etcd::client client(http, { "http://etcd:2379" });
	std::map<std::string, std::string> range = client.range("/asyncdb/node/");

	ASSERT_EQ(range.size(), 2u);
	EXPECT_EQ(range["/asyncdb/node/http://one:8080"], "http://one:8080");
	EXPECT_EQ(range["/asyncdb/node/http://two:8080"], "http://two:8080");

	// The end of a prefix is the prefix with its last byte raised, which is how etcd is asked for
	// everything under it.
	boost::json::object body = body_of(http.sent()[0]);

	EXPECT_EQ(decoded(body, "key"), "/asyncdb/node/");
	EXPECT_EQ(decoded(body, "range_end"), "/asyncdb/node0");
}

TEST(etcd_client_test, read_nothing_when_no_key_is_there)
{
	http::fake_client http;

	http.answer("/v3/kv/range", http::answer(200, "application/json", "{\"header\":{}}"));

	etcd::client client(http, { "http://etcd:2379" });

	EXPECT_TRUE(client.range("/asyncdb/node/").empty());
}

TEST(etcd_client_test, read_nothing_when_etcd_is_not_there)
{
	http::fake_client http;
	etcd::client client(http, { "http://etcd:2379" });

	EXPECT_TRUE(client.range("/asyncdb/node/").empty());
}

TEST(etcd_client_test, revoke_a_lease)
{
	http::fake_client http;

	http.answer("/v3/lease/revoke", http::answer(200, "application/json", "{\"header\":{}}"));

	etcd::client client(http, { "http://etcd:2379" });

	EXPECT_TRUE(client.revoke(12));
	EXPECT_EQ(body_of(http.sent()[0]).at("ID").as_string(), "12");
}

namespace
{
	const std::string first = "http://etcd-1:2379";

	const std::string second = "http://etcd-2:2379";

	const std::string third = "http://etcd-3:2379";

	http::response granted()
	{
		return http::answer(200, "application/json", "{\"ID\":\"12\",\"TTL\":\"10\"}");
	}
}

// Every member of an etcd cluster answers for the whole of it, so a member that is not there is a
// reason to ask the next one rather than to give up on the cluster.
TEST(etcd_client_test, ask_the_next_member_when_one_is_not_there)
{
	http::fake_client http;

	http.answer(second, granted());

	etcd::client client(http, { first, second });

	EXPECT_TRUE(client.grant_lease(10).has_value());
	EXPECT_EQ(http.sent_to(first).size(), 1u);
	EXPECT_EQ(http.sent_to(second).size(), 1u);
}

TEST(etcd_client_test, ask_the_next_member_when_one_is_not_serving)
{
	http::fake_client http;

	// Unavailable is what a member that is up but has no leader answers.
	http.answer(first, http::answer(503, "application/json", "{\"error\":\"etcdserver: no leader\"}"));
	http.answer(second, granted());

	etcd::client client(http, { first, second });

	EXPECT_TRUE(client.grant_lease(10).has_value());
	EXPECT_EQ(http.sent_to(second).size(), 1u);
}

// A refusal is the answer the whole cluster would give — a lease that is not there is not there
// on any member — so asking the rest would only be slower.
TEST(etcd_client_test, take_a_refusal_from_the_member_that_gave_it)
{
	http::fake_client http;

	http.answer(first, http::answer(404, "application/json", "{\"error\":\"etcdserver: requested lease not found\"}"));
	http.answer(second, granted());

	etcd::client client(http, { first, second });

	EXPECT_FALSE(client.grant_lease(10).has_value());
	EXPECT_EQ(http.sent_to(first).size(), 1u);
	EXPECT_TRUE(http.sent_to(second).empty());
}

TEST(etcd_client_test, stay_with_the_member_that_answered)
{
	http::fake_client http;

	http.answer(third, granted());

	etcd::client client(http, { first, second, third });

	EXPECT_TRUE(client.grant_lease(10).has_value());
	EXPECT_EQ(client.endpoint(), third);

	// The two that are not there are walked past once, not on every call after it.
	EXPECT_TRUE(client.grant_lease(10).has_value());
	EXPECT_EQ(http.sent_to(first).size(), 1u);
	EXPECT_EQ(http.sent_to(second).size(), 1u);
	EXPECT_EQ(http.sent_to(third).size(), 2u);
}

TEST(etcd_client_test, come_back_round_to_the_first_member)
{
	http::fake_client http;

	http.answer(second, granted());

	etcd::client client(http, { first, second });

	EXPECT_TRUE(client.grant_lease(10).has_value());
	EXPECT_EQ(client.endpoint(), second);

	// The one it settled on is asked first from now on, and the one before it is what comes next.
	EXPECT_EQ(http.sent()[0].url.find(first), 0u);
	EXPECT_EQ(http.sent()[1].url.find(second), 0u);
	EXPECT_TRUE(client.grant_lease(10).has_value());
	EXPECT_EQ(http.sent()[2].url.find(second), 0u);
}

TEST(etcd_client_test, fail_when_no_member_answers)
{
	http::fake_client http;
	etcd::client client(http, { first, second, third });

	EXPECT_FALSE(client.grant_lease(10).has_value());
	EXPECT_EQ(http.sent().size(), 3u);
}

TEST(etcd_client_test, fail_when_there_is_no_member_to_ask)
{
	http::fake_client http;
	etcd::client client(http, {});

	EXPECT_FALSE(client.grant_lease(10).has_value());
	EXPECT_TRUE(http.sent().empty());
	EXPECT_TRUE(client.endpoint().empty());
}

// Real etcd renews a lease that has gone by answering with no time to live at all rather than with
// a time to live of nothing.
TEST(etcd_client_test, fail_to_keep_a_lease_etcd_says_nothing_about)
{
	http::fake_client http;

	http.answer("/v3/lease/keepalive", http::answer(200, "application/json", "{\"result\":{\"ID\":\"12\"}}"));

	etcd::client client(http, { first });

	EXPECT_FALSE(client.keep_alive(12));
}

// Leaving is best effort: a node on its way out is stopped for good in ten seconds, so it spends
// one timeout on it rather than one for every member that is not there.
TEST(etcd_client_test, ask_one_member_to_revoke_a_lease)
{
	http::fake_client http;

	http.answer(second, http::answer(200, "application/json", "{}"));

	etcd::client client(http, { first, second, third });

	EXPECT_FALSE(client.revoke(12));
	EXPECT_EQ(http.sent().size(), 1u);
	EXPECT_EQ(http.sent_to(first).size(), 1u);
	EXPECT_TRUE(http.sent_to(second).empty());
}

TEST(etcd_client_test, revoke_a_lease_at_the_member_that_has_been_answering)
{
	http::fake_client http;

	// The first member is not there, which the calls before a shutdown have already found out.
	http.answer(second, granted());

	etcd::client client(http, { first, second });

	EXPECT_TRUE(client.grant_lease(10).has_value());
	EXPECT_EQ(client.endpoint(), second);

	http.answer(second, http::answer(200, "application/json", "{}"));

	EXPECT_TRUE(client.revoke(12));

	// The one attempt goes to the member that has been answering, not back to the dead one.
	ASSERT_EQ(http.sent_to("/v3/lease/revoke").size(), 1u);
	EXPECT_EQ(http.sent_to("/v3/lease/revoke")[0].url.find(second), 0u);
}
