#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <curl/curl.h>
#include <boost/json.hpp>

#include "cluster/partition.h"
#include "cluster/test_cluster.h"
#include "server/server.h"
#include "listening.h"

namespace
{
	size_t append(void *contents, size_t size, size_t count, void *body)
	{
		static_cast<std::string *>(body)->append(static_cast<const char *>(contents), size * count);

		return size * count;
	}

	struct answer
	{
		long code = 0;

		std::string body;

		long long content_length = 0;
	};
}

class cluster_test : public ::testing::Test
{
protected:
	cluster::test_cluster first_cluster;

	cluster::test_cluster second_cluster;

	std::shared_ptr<server::server> first;

	std::shared_ptr<server::server> second;

	std::thread first_thread;

	std::thread second_thread;

	void SetUp()
	{
		std::filesystem::remove_all("/tmp/asyncdb/");

		// RocksDB locks the directory it opens, so two servers in one process are two stores.
		first = std::make_shared<server::server>(0, 2, first_cluster, "/tmp/asyncdb/first");
		second = std::make_shared<server::server>(0, 2, second_cluster, "/tmp/asyncdb/second");

		std::vector<cluster::member> members {
			cluster::member { node(first), "" },
			cluster::member { node(second), "" }
		};

		first_cluster.join(node(first), "", members);
		second_cluster.join(node(second), "", members);

		first_thread = std::thread([server = first]() { server->serve(); });
		second_thread = std::thread([server = second]() { server->serve(); });

		// The ports are opened by serve() and not by the constructors, so they are waited for
		// rather than assumed — and each server forwards to the other, so both have to be there
		// before either is asked for anything.
		server::wait_until_listening(first->port());
		server::wait_until_listening(second->port());
	}

	void TearDown()
	{
		first->close();
		second->close();
		first_thread.join();
		second_thread.join();

		first = nullptr;
		second = nullptr;
	}

	std::string node(const std::shared_ptr<server::server> &server) const
	{
		return "http://localhost:" + std::to_string(server->port());
	}

	answer request(
		const std::shared_ptr<server::server> &server,
		const std::string &method,
		const std::string &path,
		const std::string &body,
		bool forwarded = false,
		int64_t term = 0)
	{
		CURL *curl = curl_easy_init();
		answer answer;
		struct curl_slist *headers = NULL;

		headers = curl_slist_append(headers, "Connection: close");

		// The term a leader ordered a write in, which the node it reaches fences on.
		if (term != 0)
		{
			headers = curl_slist_append(
				headers, ("X-Asyncdb-Term: " + std::to_string(term)).c_str());
		}

		// A request that says it was forwarded is served where it lands, which is how a test asks
		// one node what it holds itself rather than what the cluster holds.
		if (forwarded)
		{
			headers = curl_slist_append(headers, "X-Asyncdb-Forwarded: true");
		}

		curl_easy_setopt(curl, CURLOPT_URL, (node(server) + path).c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &answer.body);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

		if (method == "HEAD")
		{
			curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
		}
		else if (method != "GET")
		{
			curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
		}

		curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &answer.code);

		curl_off_t content_length = 0;

		curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);

		answer.content_length = content_length;

		curl_easy_cleanup(curl);
		curl_slist_free_all(headers);

		return answer;
	}

	answer get(const std::shared_ptr<server::server> &server, const std::string &path)
	{
		return request(server, "GET", path, "");
	}

	// The same two servers, with one of them ordering the writes of every partition.
	void led_by(const std::shared_ptr<server::server> &leader, int64_t term)
	{
		first_cluster.led_by(node(leader), term);
		second_cluster.led_by(node(leader), term);
	}

	// The same two servers, one in each of two zones, which is a cluster keeping a copy of every
	// record in both of them rather than one copy between them.
	void zone_the_cluster()
	{
		std::vector<cluster::member> members {
			cluster::member { node(first), "one" },
			cluster::member { node(second), "two" }
		};

		first_cluster.join(node(first), "one", members);
		second_cluster.join(node(second), "two", members);
	}

	// The same two servers and the same split between them, said again. Nothing about who owns
	// what moves — the hash is over the node addresses, and those have not changed — but the
	// membership is a different one, which is what a node watches for and what starts it moving
	// the records whose owner moved.
	void name_the_zone(const std::string &zone)
	{
		std::vector<cluster::member> members {
			cluster::member { node(first), zone },
			cluster::member { node(second), zone }
		};

		first_cluster.join(node(first), zone, members);
		second_cluster.join(node(second), zone, members);
	}

	// What a node holds in its own store, which a request that says it was forwarded is answered
	// out of. Reading it through the cluster would find the copy on the other node instead.
	bool holds(const std::shared_ptr<server::server> &server, const std::string &key)
	{
		return request(server, "GET", "/table/account/key/" + key, "", true).code == 200;
	}

	// A pass runs on a tick of its own and takes as long as the round trips in it, so what is
	// waited for is that it happened rather than that it happened by now.
	bool holds_within(const std::shared_ptr<server::server> &server, const std::string &key, bool held)
	{
		std::chrono::steady_clock::time_point deadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(30);

		while (std::chrono::steady_clock::now() < deadline)
		{
			if (holds(server, key) == held)
			{
				return true;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}

		return false;
	}

	// The rule the cluster itself owns by: a key belongs to a partition, and it is the partition
	// that is hashed against the nodes.
	std::shared_ptr<server::server> &owner(const std::string &key)
	{
		std::string partition = cluster::partition_name(cluster::partition_of(key));

		return cluster::owner_of(partition, { node(first), node(second) }) == node(first) ? first : second;
	}

	std::shared_ptr<server::server> &stranger(const std::string &key)
	{
		return owner(key) == first ? second : first;
	}

	std::vector<std::string> keys(const answer &answer)
	{
		boost::json::array records = boost::json::parse(answer.body).as_object().at("records").as_array();
		std::vector<std::string> keys;

		for (size_t i = 0; i < records.size(); i++)
		{
			keys.push_back(std::string(records[i].as_object().at("key").as_string()));
		}

		return keys;
	}
};

// The cluster a server routes through is the one it joins: a server that registered itself
// somewhere else would be a member of a membership it never reads.
TEST_F(cluster_test, joins_the_cluster_it_was_given)
{
	EXPECT_TRUE(first_cluster.joined());
	EXPECT_TRUE(second_cluster.joined());
}

TEST_F(cluster_test, a_table_is_created_on_every_node)
{
	EXPECT_EQ(request(first, "PUT", "/table/account", "{}").code, 201);

	EXPECT_EQ(get(first, "/table/account").code, 200);
	EXPECT_EQ(get(second, "/table/account").code, 200);
}

TEST_F(cluster_test, a_record_is_written_to_the_node_that_owns_its_key)
{
	request(first, "PUT", "/table/account", "{}");
	request(stranger("4821"), "PUT", "/table/account/key/4821", "a value");

	// Both nodes answer with the value, and only one of them holds it.
	EXPECT_EQ(get(first, "/table/account/key/4821").body, "a value");
	EXPECT_EQ(get(second, "/table/account/key/4821").body, "a value");
	EXPECT_EQ(request(owner("4821"), "GET", "/table/account/key/4821", "", true).body, "a value");
	EXPECT_EQ(request(stranger("4821"), "GET", "/table/account/key/4821", "", true).code, 404);
}

// What the split is for: every record of one partition key is in one partition, so one node holds
// them all however many there are, and a record and the records derived from it are one hop.
TEST_F(cluster_test, records_of_one_partition_key_are_held_by_one_node)
{
	request(first, "PUT", "/table/account", "{}");

	for (size_t i = 0; i < 20; i++)
	{
		std::string year = std::to_string(2000 + i);

		request(first, "PUT", "/table/account/key/4821/" + year, "a year of it");

		EXPECT_TRUE(holds(owner("4821"), "4821/" + year));
		EXPECT_FALSE(holds(stranger("4821"), "4821/" + year));
	}
}

// An empty value is a value, and it is told from a missing key by the status, which has to survive
// the hop to the node that owns the key.
TEST_F(cluster_test, an_empty_value_survives_the_hop)
{
	request(first, "PUT", "/table/account", "{}");

	EXPECT_EQ(request(stranger("4821"), "PUT", "/table/account/key/4821", "").code, 204);

	answer read = get(stranger("4821"), "/table/account/key/4821");

	EXPECT_EQ(read.code, 200);
	EXPECT_EQ(read.body, "");
	EXPECT_EQ(get(stranger("7203"), "/table/account/key/7203").code, 404);
}

TEST_F(cluster_test, a_value_larger_than_one_packet_survives_the_hop)
{
	request(first, "PUT", "/table/account", "{}");

	std::string value(200000, 'x');

	EXPECT_EQ(request(stranger("4821"), "PUT", "/table/account/key/4821", value).code, 204);
	EXPECT_EQ(get(stranger("4821"), "/table/account/key/4821").body, value);
}

TEST_F(cluster_test, a_record_is_deleted_on_the_node_that_owns_its_key)
{
	request(first, "PUT", "/table/account", "{}");
	request(first, "PUT", "/table/account/key/4821", "a value");

	EXPECT_EQ(request(stranger("4821"), "DELETE", "/table/account/key/4821", "").code, 204);
	EXPECT_EQ(get(first, "/table/account/key/4821").code, 404);
}

TEST_F(cluster_test, the_size_of_a_record_is_answered_by_the_node_that_owns_it)
{
	request(first, "PUT", "/table/account", "{}");
	request(first, "PUT", "/table/account/key/4821", "a value");

	answer head = request(stranger("4821"), "HEAD", "/table/account/key/4821", "");

	EXPECT_EQ(head.code, 200);
	EXPECT_EQ(head.content_length, 7);
	EXPECT_EQ(head.body, "");
}

// The value never crosses the hop: the node that owns the key answers the HEAD with a HEAD, and
// the node that was asked passes the length on without ever having read the value.
TEST_F(cluster_test, the_size_of_an_empty_record_is_answered_as_nothing)
{
	request(first, "PUT", "/table/account", "{}");
	request(first, "PUT", "/table/account/key/4821", "");

	answer head = request(stranger("4821"), "HEAD", "/table/account/key/4821", "");

	EXPECT_EQ(head.code, 200);
	EXPECT_EQ(head.content_length, 0);
}

TEST_F(cluster_test, a_head_of_a_record_that_is_not_there_is_not_found)
{
	request(first, "PUT", "/table/account", "{}");

	EXPECT_EQ(request(stranger("4821"), "HEAD", "/table/account/key/4821", "").code, 404);
}

TEST_F(cluster_test, a_key_that_holds_punctuation_of_a_url_is_one_key)
{
	request(first, "PUT", "/table/account", "{}");
	request(first, "PUT", "/table/account/key/a%2Fb", "a value");

	EXPECT_EQ(get(second, "/table/account/key/a%2Fb").body, "a value");
	EXPECT_EQ(get(second, "/table/account/key/a").code, 404);
}

TEST_F(cluster_test, a_scan_answers_the_keys_of_every_node_in_order)
{
	request(first, "PUT", "/table/account", "{}");

	std::vector<std::string> written;

	for (size_t i = 0; i < 20; i++)
	{
		std::string key = "row-" + std::to_string(i);

		request(first, "PUT", "/table/account/key/" + key, "a value");
		written.push_back(key);
	}

	std::sort(written.begin(), written.end());

	EXPECT_EQ(keys(get(first, "/table/account/key")), written);
	EXPECT_EQ(keys(get(second, "/table/account/key")), written);

	// The keys really are spread: neither node holds all of them.
	EXPECT_LT(keys(request(first, "GET", "/table/account/key", "", true)).size(), written.size());
	EXPECT_LT(keys(request(second, "GET", "/table/account/key", "", true)).size(), written.size());
}

TEST_F(cluster_test, a_scan_is_paged_across_the_nodes)
{
	request(first, "PUT", "/table/account", "{}");

	for (size_t i = 0; i < 20; i++)
	{
		request(first, "PUT", "/table/account/key/row-" + std::to_string(i), "a value");
	}

	std::vector<std::string> paged;
	std::string query = "?limit=3";

	for (size_t i = 0; i < 10; i++)
	{
		answer page = get(first, "/table/account/key" + query);
		std::vector<std::string> keys_of_page = keys(page);

		paged.insert(paged.end(), keys_of_page.begin(), keys_of_page.end());

		boost::json::object body = boost::json::parse(page.body).as_object();

		if (!body.contains("next"))
		{
			break;
		}

		query = "?limit=3&cursor=" + std::string(body.at("next").as_string());
	}

	EXPECT_EQ(paged, keys(get(first, "/table/account/key")));
	EXPECT_EQ(paged.size(), 20u);
}

TEST_F(cluster_test, a_range_is_deleted_on_every_node)
{
	request(first, "PUT", "/table/account", "{}");

	for (size_t i = 0; i < 20; i++)
	{
		request(first, "PUT", "/table/account/key/row-" + std::to_string(i), "a value");
	}

	EXPECT_EQ(request(second, "DELETE", "/table/account/key?prefix=row-1", "").code, 204);

	std::vector<std::string> left = keys(get(first, "/table/account/key"));

	for (size_t i = 0; i < left.size(); i++)
	{
		EXPECT_NE(left[i].rfind("row-1", 0), 0u);
	}

	EXPECT_EQ(left.size(), 9u);
}

TEST_F(cluster_test, a_table_is_deleted_on_every_node)
{
	request(first, "PUT", "/table/account", "{}");
	request(first, "PUT", "/table/account/key/4821", "a value");

	EXPECT_EQ(request(second, "DELETE", "/table/account", "").code, 204);
	EXPECT_EQ(get(first, "/table/account").code, 404);
	EXPECT_EQ(get(second, "/table/account").code, 404);
}

TEST_F(cluster_test, the_health_of_an_instance_names_the_nodes_of_its_cluster)
{
	boost::json::object health = boost::json::parse(get(first, "/health").body).as_object();

	ASSERT_TRUE(health.contains("nodes"));
	EXPECT_EQ(health.at("nodes").as_array().size(), 2u);
}

// Two nodes in two zones: every record is on both of them, which is what a zoned cluster is for.
TEST_F(cluster_test, a_record_is_written_to_the_node_in_every_zone)
{
	zone_the_cluster();

	request(first, "PUT", "/table/account", "{}");

	EXPECT_EQ(request(first, "PUT", "/table/account/key/4821", "a value").code, 204);

	// Asked as forwarded, a node answers out of its own RocksDB and not out of the cluster's, so
	// this is both nodes saying they hold the record themselves.
	EXPECT_EQ(request(first, "GET", "/table/account/key/4821", "", true).body, "a value");
	EXPECT_EQ(request(second, "GET", "/table/account/key/4821", "", true).body, "a value");
}

TEST_F(cluster_test, a_record_is_deleted_in_every_zone)
{
	zone_the_cluster();

	request(first, "PUT", "/table/account", "{}");
	request(first, "PUT", "/table/account/key/4821", "a value");

	EXPECT_EQ(request(second, "DELETE", "/table/account/key/4821", "").code, 204);
	EXPECT_EQ(request(first, "GET", "/table/account/key/4821", "", true).code, 404);
	EXPECT_EQ(request(second, "GET", "/table/account/key/4821", "", true).code, 404);
}

// The copies are the same key twice, and a scan of the cluster answers it once.
TEST_F(cluster_test, a_scan_answers_a_record_that_is_in_every_zone_once)
{
	zone_the_cluster();

	request(first, "PUT", "/table/account", "{}");
	request(first, "PUT", "/table/account/key/4821", "a value");
	request(second, "PUT", "/table/account/key/4822", "another value");

	EXPECT_EQ(keys(get(first, "/table/account/key")), (std::vector<std::string> { "4821", "4822" }));
	EXPECT_EQ(keys(get(second, "/table/account/key")), (std::vector<std::string> { "4821", "4822" }));
}

// A zone that is gone is a copy that is gone, and the record is read from the zone that is left.
TEST_F(cluster_test, a_record_is_read_from_the_zone_that_is_still_there)
{
	zone_the_cluster();

	request(first, "PUT", "/table/account", "{}");
	request(first, "PUT", "/table/account/key/4821", "a value");

	second->close();
	second_thread.join();

	answer answered = get(first, "/table/account/key/4821");

	EXPECT_EQ(answered.code, 200);
	EXPECT_EQ(answered.body, "a value");

	// The test stops the servers it starts, and this one has stopped already.
	second_thread = std::thread([]() {});
}

// The copies are asked in turn, so a node that is not there is a copy passed over rather than the
// answer to the request.
TEST_F(cluster_test, a_record_is_read_from_the_next_zone_when_a_node_does_not_answer)
{
	zone_the_cluster();

	request(first, "PUT", "/table/account", "{}");
	request(first, "PUT", "/table/account/key/4821", "a value");

	// A membership naming a zone whose node is not there and not naming this node at all: the
	// first node holds no copy of its own, and the copy it asks for first answers nothing.
	first_cluster.join(node(first), "one", {
		cluster::member { "http://localhost:1", "one" },
		cluster::member { node(second), "two" }
	});

	answer answered = get(first, "/table/account/key/4821");

	EXPECT_EQ(answered.code, 200);
	EXPECT_EQ(answered.body, "a value");
}

// The whole of what a scan across zones is for: this node's zone holds every key, so the scan is
// answered out of it alone and the zone that is gone is never asked.
TEST_F(cluster_test, a_scan_answers_every_record_when_the_other_zone_is_gone)
{
	zone_the_cluster();

	request(first, "PUT", "/table/account", "{}");
	request(first, "PUT", "/table/account/key/4821", "a value");
	request(first, "PUT", "/table/account/key/4822", "another value");

	second->close();
	second_thread.join();

	EXPECT_EQ(keys(get(first, "/table/account/key")), (std::vector<std::string> { "4821", "4822" }));

	// The test stops the servers it starts, and this one has stopped already.
	second_thread = std::thread([]() {});
}

// A write that lands on a node which does not lead the partition travels to the one that does, and
// the leader writes every copy from there.
TEST_F(cluster_test, a_write_is_ordered_by_the_node_that_leads_the_partition)
{
	zone_the_cluster();
	led_by(second, 7);

	request(first, "PUT", "/table/account", "{}");

	EXPECT_EQ(request(first, "PUT", "/table/account/key/4821", "a value").code, 204);

	// Both nodes hold it, and the one that ordered it is the second.
	EXPECT_EQ(request(first, "GET", "/table/account/key/4821", "", true).body, "a value");
	EXPECT_EQ(request(second, "GET", "/table/account/key/4821", "", true).body, "a value");
}

// The fence, over a real socket: the term travels in a header, and a write ordered in a term the
// node has moved past is refused rather than applied behind the leader that replaced it.
TEST_F(cluster_test, a_write_ordered_in_a_term_that_has_passed_is_refused)
{
	zone_the_cluster();
	led_by(second, 7);

	request(first, "PUT", "/table/account", "{}");

	answer stale = request(first, "PUT", "/table/account/key/4821", "a value", true, 1);

	EXPECT_EQ(stale.code, 409);
	EXPECT_NE(stale.body.find("stale_leader"), std::string::npos);
	EXPECT_EQ(request(first, "GET", "/table/account/key/4821", "", true).code, 404);

	// The term that stands is applied.
	EXPECT_EQ(request(first, "PUT", "/table/account/key/4821", "a value", true, 7).code, 204);
	EXPECT_EQ(request(first, "GET", "/table/account/key/4821", "", true).body, "a value");
}

// A cluster with no leadership at all — no zones, or no etcd to elect through — writes from
// wherever the write landed to every copy.
TEST_F(cluster_test, a_write_is_unordered_when_no_node_leads_anything)
{
	zone_the_cluster();

	request(first, "PUT", "/table/account", "{}");

	EXPECT_EQ(request(first, "PUT", "/table/account/key/4821", "a value").code, 204);
	EXPECT_EQ(request(first, "GET", "/table/account/key/4821", "", true).body, "a value");
	EXPECT_EQ(request(second, "GET", "/table/account/key/4821", "", true).body, "a value");
}

// The membership moved, so the records whose owner moved with it move too. Both halves are one
// pass: a node fetches what it has been handed and gives up what has been taken from it, and the
// second is gated on the first having happened somewhere else.

TEST_F(cluster_test, a_node_gives_up_a_record_it_no_longer_owns)
{
	request(first, "PUT", "/table/account", "{}");

	// Written where it stands rather than where it belongs, so that both nodes hold a key one of
	// them owns. That is the state a cluster is left in by a node joining the zone.
	request(first, "PUT", "/table/account/key/4821", "a value", true);
	request(second, "PUT", "/table/account/key/4821", "a value", true);

	ASSERT_TRUE(holds(owner("4821"), "4821"));
	ASSERT_TRUE(holds(stranger("4821"), "4821"));

	name_the_zone("one");

	// The node that does not own it lets it go, and the node that does keeps it.
	EXPECT_TRUE(holds_within(stranger("4821"), "4821", false));
	EXPECT_TRUE(holds(owner("4821"), "4821"));
}

// Both halves of one zone's copy in the order they have to happen in. The record is on the node
// that does not own it and nowhere else, so the copy that is given up is the one the fetch has
// already made: a delete that ran first would be the record.
TEST_F(cluster_test, a_record_moves_to_the_node_that_owns_it_and_then_off_the_one_that_does_not)
{
	request(first, "PUT", "/table/account", "{}");

	request(stranger("4821"), "PUT", "/table/account/key/4821", "a value", true);

	ASSERT_FALSE(holds(owner("4821"), "4821"));

	name_the_zone("one");

	EXPECT_TRUE(holds_within(owner("4821"), "4821", true));
	EXPECT_EQ(request(owner("4821"), "GET", "/table/account/key/4821", "", true).body, "a value");

	// And only then is the copy that is nobody's given up, which leaves the zone one copy, on the
	// node that owns it.
	EXPECT_TRUE(holds_within(stranger("4821"), "4821", false));
}

TEST_F(cluster_test, a_node_fetches_a_record_it_owns_and_holds_nothing_for)
{
	zone_the_cluster();

	request(first, "PUT", "/table/account", "{}");

	// One zone holds the record and the other does not, which is a copy short: every zone holds
	// one node that owns this key, and here one of them has nothing behind it.
	request(second, "PUT", "/table/account/key/4821", "a value", true);

	ASSERT_FALSE(holds(first, "4821"));

	std::vector<cluster::member> members {
		cluster::member { node(first), "three" },
		cluster::member { node(second), "four" }
	};

	first_cluster.join(node(first), "three", members);
	second_cluster.join(node(second), "four", members);

	EXPECT_TRUE(holds_within(first, "4821", true));
	EXPECT_EQ(request(first, "GET", "/table/account/key/4821", "", true).body, "a value");
}
