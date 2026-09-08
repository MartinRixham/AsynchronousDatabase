#ifndef CLUSTER_CLUSTER_H
#define CLUSTER_CLUSTER_H

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "router/request.h"
#include "router/response.h"
#include "member.h"

namespace cluster
{
	constexpr char forwarded_header[] = "X-Asyncdb-Forwarded";

	constexpr char term_header[] = "X-Asyncdb-Term";

	struct placement
	{
		bool local = true;

		std::vector<std::string> nodes;
	};

	struct leadership
	{
		bool known = false;

		bool local = false;

		std::string node;

		int64_t term = 0;
	};

	// The seam over the other instances, in the way that repository::repository is the seam over
	// the store. A key belongs to one node in each zone, and a request for a key this node holds no
	// copy of is answered by a node that does.
	class cluster
	{
	public:
		virtual std::vector<member> members() const = 0;

		virtual placement replicas(const std::string &key) const = 0;

		virtual std::vector<std::string> peers() const = 0;

		virtual std::vector<std::vector<std::string>> zones() const = 0;

		virtual std::optional<leadership> leader(const std::string &key) const = 0;

		virtual size_t leads() const = 0;

		virtual bool accept(const std::string &key, int64_t term) = 0;

		virtual router::response send(const std::string &node, const router::request &request) const = 0;

		// A cluster that can ask them at once asks them at once. A write is not done until every
		// copy has taken it, and asking one after another holds the thread serving the write for a
		// round trip each — and a thread waiting on another node cannot answer anything else, its
		// health check included.
		virtual std::optional<router::response> send_all(
			const std::vector<std::string> &node_list,
			const router::request &request) const = 0;
	};

	std::optional<router::response> refusal(const std::vector<router::response> &answers);
}

#endif
