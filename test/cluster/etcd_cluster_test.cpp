#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <boost/beast/http.hpp>

#include "base64/base64.h"
#include "cluster/etcd_cluster.h"
#include "cluster/partition.h"
#include "http/fake_http_client.h"

namespace
{
	const std::string one = "http://asyncdb-1:8080";

	const std::string two = "http://asyncdb-2:8080";

	cluster::config configuration(const std::string &node)
	{
		cluster::config config;

		config.endpoints = { "http://etcd:2379" };
		config.node = node;

		return config;
	}

	cluster::config configuration(const std::string &node, const std::string &zone)
	{
		cluster::config config = configuration(node);

		config.zone = zone;

		return config;
	}

	// The membership as etcd holds it: a node writes where it answers and which zone it is in.
	std::string membership(const std::vector<cluster::member> &nodes)
	{
		boost::json::array kvs;

		for (size_t i = 0; i < nodes.size(); i++)
		{
			boost::json::object registration {
				{ "node", nodes[i].node },
				{ "zone", nodes[i].zone }
			};

			kvs.push_back(boost::json::object {
				{ "key", base64::encode("/asyncdb/node/" + nodes[i].node) },
				{ "value", base64::encode(boost::json::serialize(registration)) }
			});
		}

		return boost::json::serialize(boost::json::object { { "kvs", kvs } });
	}

	std::vector<cluster::member> zoneless(const std::vector<std::string> &nodes)
	{
		std::vector<cluster::member> members;

		for (size_t i = 0; i < nodes.size(); i++)
		{
			members.push_back(cluster::member { nodes[i], "" });
		}

		return members;
	}

	// The addresses of the membership, which is what most of this is about.
	std::vector<std::string> names(const std::vector<cluster::member> &members)
	{
		std::vector<std::string> names;

		for (size_t i = 0; i < members.size(); i++)
		{
			names.push_back(members[i].node);
		}

		return names;
	}

	void answer_etcd(http::fake_client *http, const std::vector<cluster::member> &nodes)
	{
		http->answer("/v3/lease/grant", http::answer(200, "application/json", "{\"ID\":\"12\",\"TTL\":\"10\"}"));
		http->answer("/v3/lease/keepalive", http::answer(200, "application/json", "{\"result\":{\"TTL\":\"10\"}}"));
		http->answer("/v3/lease/revoke", http::answer(200, "application/json", "{}"));
		http->answer("/v3/kv/put", http::answer(200, "application/json", "{}"));
		http->answer("/v3/kv/range", http::answer(200, "application/json", membership(nodes)));
	}

	void answer_etcd(http::fake_client *http, const std::vector<std::string> &nodes)
	{
		answer_etcd(http, zoneless(nodes));
	}

	router::request request(boost::beast::http::verb method, const std::vector<std::string> &path)
	{
		return { method, path, "", "", false };
	}
}

TEST(etcd_cluster_test, stand_alone_when_no_etcd_is_configured)
{
	http::fake_client http;
	cluster::config config;

	config.node = one;

	cluster::etcd_cluster cluster(config, http);

	cluster.start();

	EXPECT_TRUE(http.sent().empty());
	EXPECT_TRUE(cluster.members().empty());
	EXPECT_TRUE(cluster.peers().empty());
	EXPECT_TRUE(cluster.replicas("key").local);
	EXPECT_TRUE(cluster.replicas("key").nodes.empty());
}

TEST(etcd_cluster_test, stand_alone_when_no_node_is_configured)
{
	http::fake_client http;
	cluster::config config;

	config.endpoints = { "http://etcd:2379" };

	cluster::etcd_cluster cluster(config, http);

	cluster.start();

	EXPECT_TRUE(http.sent().empty());
	EXPECT_TRUE(cluster.members().empty());
}

TEST(etcd_cluster_test, register_the_node_and_read_the_membership)
{
	http::fake_client http;

	answer_etcd(&http, { one, two });

	cluster::etcd_cluster cluster(configuration(one), http);

	cluster.start();

	ASSERT_EQ(http.sent_to("/v3/kv/put").size(), 1u);

	boost::json::object put = boost::json::parse(http.sent_to("/v3/kv/put")[0].body).as_object();
	std::string key;

	base64::decode(std::string(put.at("key").as_string()), &key);

	EXPECT_EQ(key, "/asyncdb/node/" + one);
	EXPECT_EQ(put.at("lease").as_string(), "12");
	EXPECT_EQ(names(cluster.members()), std::vector<std::string>({ one, two }));
	EXPECT_EQ(cluster.peers(), std::vector<std::string>({ two }));

	cluster.stop();

	EXPECT_EQ(http.sent_to("/v3/lease/revoke").size(), 1u);
}

// A node that cannot reach etcd is a cluster of one rather than a node that refuses to answer, so
// it keeps serving the keys it holds.
TEST(etcd_cluster_test, be_a_member_of_its_own_cluster_when_etcd_is_not_there)
{
	http::fake_client http;
	cluster::etcd_cluster cluster(configuration(one), http);

	cluster.start();

	EXPECT_EQ(names(cluster.members()), std::vector<std::string>({ one }));
	EXPECT_TRUE(cluster.peers().empty());
	EXPECT_TRUE(cluster.replicas("key").local);

	cluster.stop();
}

TEST(etcd_cluster_test, hold_every_key_when_no_other_node_is_registered)
{
	http::fake_client http;

	answer_etcd(&http, { one });

	cluster::etcd_cluster cluster(configuration(one), http);

	cluster.start();

	for (size_t i = 0; i < 100; i++)
	{
		EXPECT_TRUE(cluster.replicas("account/" + std::to_string(i)).local);
	}

	cluster.stop();
}

TEST(etcd_cluster_test, name_the_node_that_holds_a_key)
{
	http::fake_client http;

	answer_etcd(&http, { one, two });

	cluster::etcd_cluster cluster(configuration(one), http);

	cluster.start();

	size_t forwarded = 0;

	for (size_t i = 0; i < 100; i++)
	{
		std::string key = "account/" + std::to_string(i);
		cluster::placement where = cluster.replicas(key);

		// A cluster of one zone keeps one copy, so a key is either this node's own or the other
		// node's, and never both.
		if (cluster::owner_of(key, { one, two }) == one)
		{
			EXPECT_TRUE(where.local);
			EXPECT_TRUE(where.nodes.empty());
		}
		else
		{
			EXPECT_FALSE(where.local);
			EXPECT_EQ(where.nodes, std::vector<std::string>({ two }));

			forwarded++;
		}
	}

	EXPECT_GT(forwarded, 0u);

	cluster.stop();
}

// A node knows which zone it is in and no other node does, so it registers the two together.
TEST(etcd_cluster_test, register_the_zone_the_node_is_in)
{
	http::fake_client http;

	answer_etcd(&http, { cluster::member { one, "a" } });

	cluster::etcd_cluster cluster(configuration(one, "a"), http);

	cluster.start();

	ASSERT_EQ(http.sent_to("/v3/kv/put").size(), 1u);

	boost::json::object put = boost::json::parse(http.sent_to("/v3/kv/put")[0].body).as_object();
	std::string value;

	base64::decode(std::string(put.at("value").as_string()), &value);

	boost::json::object registered = boost::json::parse(value).as_object();

	EXPECT_EQ(registered.at("node").as_string(), one);
	EXPECT_EQ(registered.at("zone").as_string(), "a");

	cluster.stop();
}

// A node registered by a version that had never heard of zones is a node in no zone rather than a
// node the membership does not have, so a cluster half way through an upgrade still partitions.
TEST(etcd_cluster_test, read_a_node_that_registered_nothing_but_its_address)
{
	http::fake_client http;

	answer_etcd(&http, { one, two });

	cluster::etcd_cluster cluster(configuration(one), http);

	cluster.start();

	std::string legacy = boost::json::serialize(boost::json::object {
		{ "kvs", boost::json::array {
			boost::json::object {
				{ "key", base64::encode("/asyncdb/node/" + two) },
				{ "value", base64::encode(two) }
			}
		} }
	});

	http.answer("/v3/kv/range", http::answer(200, "application/json", legacy));

	EXPECT_EQ(names(cluster.members()), std::vector<std::string>({ one, two }));

	cluster.stop();
}

// Every zone holds a copy, so a cluster of three zones of one node holds every key on every node.
TEST(etcd_cluster_test, keep_a_copy_of_every_key_in_every_zone)
{
	http::fake_client http;
	const std::string three = "http://asyncdb-3:8080";

	answer_etcd(&http, {
		cluster::member { one, "a" },
		cluster::member { two, "b" },
		cluster::member { three, "c" }
	});

	cluster::etcd_cluster cluster(configuration(one, "a"), http);

	cluster.start();

	for (size_t i = 0; i < 100; i++)
	{
		cluster::placement where = cluster.replicas("account/" + std::to_string(i));

		EXPECT_TRUE(where.local);
		EXPECT_EQ(where.nodes, std::vector<std::string>({ two, three }));
	}

	cluster.stop();
}

// Two zones of two nodes: the key is partitioned inside each zone, and one copy of it is in each.
TEST(etcd_cluster_test, keep_one_copy_of_a_key_in_each_zone)
{
	http::fake_client http;
	const std::string three = "http://asyncdb-3:8080";
	const std::string four = "http://asyncdb-4:8080";
	std::vector<std::string> near { one, two };
	std::vector<std::string> far { three, four };

	answer_etcd(&http, {
		cluster::member { one, "a" },
		cluster::member { two, "a" },
		cluster::member { three, "b" },
		cluster::member { four, "b" }
	});

	cluster::etcd_cluster cluster(configuration(one, "a"), http);

	cluster.start();

	size_t held = 0;
	size_t asked = 0;

	for (size_t i = 0; i < 100; i++)
	{
		std::string key = "account/" + std::to_string(i);
		cluster::placement where = cluster.replicas(key);

		if (cluster::owner_of(key, near) == one)
		{
			EXPECT_TRUE(where.local);
			EXPECT_EQ(where.nodes, std::vector<std::string>({ cluster::owner_of(key, far) }));

			held++;
		}
		else
		{
			// The copy in this node's own zone is the near one, so it is the one asked first.
			EXPECT_FALSE(where.local);
			EXPECT_EQ(where.nodes, std::vector<std::string>({ two, cluster::owner_of(key, far) }));

			asked++;
		}
	}

	EXPECT_GT(held, 0u);
	EXPECT_GT(asked, 0u);

	cluster.stop();
}

TEST(etcd_cluster_test, send_a_request_to_another_node)
{
	http::fake_client http;

	answer_etcd(&http, { one, two });
	http.answer(two, http::answer(200, "text/plain; charset=utf-8", "a value"));

	cluster::etcd_cluster cluster(configuration(one), http);
	router::request forwarded = request(boost::beast::http::verb::get, { "table", "account", "key", "4821" });
	router::response response = cluster.send(two, forwarded);

	EXPECT_EQ(response.status, boost::beast::http::status::ok);
	EXPECT_EQ(response.text, "a value");

	ASSERT_EQ(http.sent_to(two).size(), 1u);

	http::request sent = http.sent_to(two)[0];

	EXPECT_EQ(sent.method, "GET");
	EXPECT_EQ(sent.url, two + "/table/account/key/4821");
	ASSERT_EQ(sent.headers.size(), 1u);
	EXPECT_EQ(sent.headers[0], "X-Asyncdb-Forwarded: true");
}

TEST(etcd_cluster_test, send_a_key_that_holds_punctuation_of_a_url)
{
	http::fake_client http;

	http.answer(two, http::answer(200, "text/plain; charset=utf-8", "a value"));

	cluster::etcd_cluster cluster(configuration(one), http);

	cluster.send(two, request(boost::beast::http::verb::get, { "table", "account", "key", "a/b?c" }));

	EXPECT_EQ(http.sent()[0].url, two + "/table/account/key/a%2Fb%3Fc");
}

TEST(etcd_cluster_test, send_a_query_as_it_stands)
{
	http::fake_client http;

	http.answer(two, http::answer(200, "application/json", "{\"records\":[]}"));

	cluster::etcd_cluster cluster(configuration(one), http);
	router::request scan { boost::beast::http::verb::get, { "table", "account", "key" }, "limit=10&from=a", "", false };

	cluster.send(two, scan);

	EXPECT_EQ(http.sent()[0].url, two + "/table/account/key?limit=10&from=a");
}

TEST(etcd_cluster_test, send_a_body)
{
	http::fake_client http;

	http.answer(two, http::answer(204, "", ""));

	cluster::etcd_cluster cluster(configuration(one), http);
	router::request write {
		boost::beast::http::verb::put,
		{ "table", "account", "key", "4821" },
		"",
		"a value",
		false
	};

	router::response response = cluster.send(two, write);

	EXPECT_EQ(response.status, boost::beast::http::status::no_content);
	EXPECT_TRUE(response.content_type.empty());
	EXPECT_EQ(http.sent()[0].method, "PUT");
	EXPECT_EQ(http.sent()[0].body, "a value");
}

// The node that owns the key is the only one that can say how large the value is, and the session
// this answer belongs to is what leaves the body out again.
TEST(etcd_cluster_test, send_a_head_request_as_a_get)
{
	http::fake_client http;

	http.answer(two, http::answer(200, "text/plain; charset=utf-8", "a value"));

	cluster::etcd_cluster cluster(configuration(one), http);
	router::response response =
		cluster.send(two, request(boost::beast::http::verb::head, { "table", "account", "key", "4821" }));

	EXPECT_EQ(http.sent()[0].method, "GET");
	EXPECT_EQ(response.text, "a value");
}

TEST(etcd_cluster_test, answer_a_document_as_a_document)
{
	http::fake_client http;

	http.answer(two, http::answer(404, "application/json", "{\"error\":{\"code\":\"table_not_found\"}}"));

	cluster::etcd_cluster cluster(configuration(one), http);
	router::response response =
		cluster.send(two, request(boost::beast::http::verb::get, { "table", "account", "key", "4821" }));

	EXPECT_EQ(response.status, boost::beast::http::status::not_found);
	EXPECT_EQ(response.json.at("error").as_object().at("code").as_string(), "table_not_found");
}

TEST(etcd_cluster_test, answer_a_storage_error_when_the_node_is_not_there)
{
	http::fake_client http;
	cluster::etcd_cluster cluster(configuration(one), http);
	router::response response =
		cluster.send(two, request(boost::beast::http::verb::get, { "table", "account", "key", "4821" }));

	EXPECT_EQ(response.status, boost::beast::http::status::internal_server_error);
	EXPECT_EQ(response.json.at("error").as_object().at("code").as_string(), "storage_error");
}

TEST(etcd_cluster_test, answer_a_storage_error_when_the_node_answers_with_something_else)
{
	http::fake_client http;

	http.answer(two, http::answer(200, "application/json", "not a document"));

	cluster::etcd_cluster cluster(configuration(one), http);
	router::response response =
		cluster.send(two, request(boost::beast::http::verb::get, { "table", "account", "key", "4821" }));

	EXPECT_EQ(response.status, boost::beast::http::status::internal_server_error);
}

TEST(etcd_cluster_test, register_again_when_the_lease_has_gone)
{
	http::fake_client http;

	answer_etcd(&http, { one });

	cluster::config config = configuration(one);

	// A lease of nothing means the membership is renewed on every tick of the thread, which is
	// what a node does when it has been away long enough to be dropped.
	config.lease_seconds = 1;

	cluster::etcd_cluster cluster(config, http);

	cluster.start();

	EXPECT_EQ(http.sent_to("/v3/lease/grant").size(), 1u);

	cluster.stop();
}

namespace
{
	// The environment is the process's, so what a test sets it puts back.
	class environment
	{
		std::string previous_etcd;

		std::string previous_node;

	public:
		environment():
			previous_etcd(getenv("ASYNCDB_ETCD") == NULL ? "" : getenv("ASYNCDB_ETCD")),
			previous_node(getenv("ASYNCDB_NODE") == NULL ? "" : getenv("ASYNCDB_NODE"))
		{
		}

		~environment()
		{
			set("ASYNCDB_ETCD", previous_etcd);
			set("ASYNCDB_NODE", previous_node);
		}

		environment(const environment &) = delete;

		environment &operator=(const environment &) = delete;

		static void set(const std::string &name, const std::string &value)
		{
			if (value.empty())
			{
				unsetenv(name.c_str());
			}
			else
			{
				setenv(name.c_str(), value.c_str(), 1);
			}
		}
	};
}

TEST(etcd_cluster_test, read_one_member_of_etcd_from_the_environment)
{
	environment environment;

	environment.set("ASYNCDB_ETCD", "http://etcd:2379");
	environment.set("ASYNCDB_NODE", one);

	cluster::config config = cluster::from_environment();

	EXPECT_EQ(config.endpoints, std::vector<std::string>({ "http://etcd:2379" }));
	EXPECT_EQ(config.node, one);
	EXPECT_TRUE(config.is_clustered());
}

TEST(etcd_cluster_test, read_every_member_of_etcd_from_the_environment)
{
	environment environment;

	environment.set("ASYNCDB_ETCD", "http://etcd-1:2379,http://etcd-2:2379,http://etcd-3:2379");
	environment.set("ASYNCDB_NODE", one);

	EXPECT_EQ(
		cluster::from_environment().endpoints,
		std::vector<std::string>({ "http://etcd-1:2379", "http://etcd-2:2379", "http://etcd-3:2379" }));
}

// An empty address is not an address, so a stray comma names nothing rather than naming a member
// that could never answer.
TEST(etcd_cluster_test, read_past_the_commas_that_name_nothing)
{
	environment environment;

	environment.set("ASYNCDB_ETCD", ",http://etcd-1:2379,,http://etcd-2:2379,");

	EXPECT_EQ(
		cluster::from_environment().endpoints,
		std::vector<std::string>({ "http://etcd-1:2379", "http://etcd-2:2379" }));
}

TEST(etcd_cluster_test, stand_alone_when_the_environment_says_nothing)
{
	environment environment;

	environment.set("ASYNCDB_ETCD", "");
	environment.set("ASYNCDB_NODE", "");

	cluster::config config = cluster::from_environment();

	EXPECT_TRUE(config.endpoints.empty());
	EXPECT_FALSE(config.is_clustered());
}

TEST(etcd_cluster_test, stand_alone_when_only_etcd_is_named)
{
	environment environment;

	environment.set("ASYNCDB_ETCD", "http://etcd:2379");
	environment.set("ASYNCDB_NODE", "");

	EXPECT_FALSE(cluster::from_environment().is_clustered());
}
