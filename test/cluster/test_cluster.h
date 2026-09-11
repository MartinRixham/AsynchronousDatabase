#ifndef CLUSTER_TEST_CLUSTER_H
#define CLUSTER_TEST_CLUSTER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cluster/cluster.h"
#include "cluster/forwarder.h"
#include "http/curl_client.h"

namespace cluster
{
	// Two real servers on two real ports, which are known only once they are listening, so the
	// membership is told to the cluster rather than read from etcd. Everything else — who owns a
	// key, and how a request reaches the node that does — is the cluster the server runs.
	class test_cluster : public cluster
	{
		std::string self;

		std::string zone;

		std::string leader_node;

		int64_t term = 0;

		std::vector<member> member_list;

		bool started = false;

		http::curl_client curl;

		forwarder request_forwarder;

	public:
		test_cluster();

		void join(
			const std::string &node,
			const std::string &node_zone,
			const std::vector<member> &nodes);

		// The leader is told to the cluster rather than claimed in etcd, the same way the
		// membership is: what is under test here is what a leader does, not how it is chosen.
		void led_by(const std::string &node, int64_t node_term);

		// Whether the server was given this cluster to join, which is the one it routes through.
		bool joined() const;

		// The membership is the one join() was told, so there is nothing to read, and joining and
		// leaving are recorded and do nothing else.
		void start() override;

		bool discover() override;

		void stop() override;

		std::vector<member> members() const override;

		placement replicas(const std::string &key) const override;

		partition_set holdings() const override;

		std::vector<std::string> peers() const override;

		std::vector<std::vector<std::string>> zones() const override;

		std::optional<leadership> leader(const std::string &key) const override;

		size_t leads() const override;

		// A membership a test named is one every node is in, so there is no moment of being
		// unable to order a write.
		bool is_unled() const override;

		bool accept(const std::string &key, int64_t term) override;

		router::response send(const std::string &node, const router::request &request) const override;

		// The real fan out over real sockets, so that a write reaching every copy at once is
		// exercised here rather than only in production.
		std::optional<router::response> send_all(
			const std::vector<std::string> &node_list,
			const router::request &request) const override;

		std::vector<router::response> send_each(const std::vector<enquiry> &enquiries) const override;
	};
}

#endif
