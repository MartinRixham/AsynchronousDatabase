#ifndef CLUSTER_CLUSTER_H
#define CLUSTER_CLUSTER_H

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "router/request.h"
#include "router/response.h"
#include "member.h"
#include "partition.h"

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

	// Where the keys of each partition live, worked out once a partition instead of once a key.
	//
	// A pass that moves records asks where every key of a store belongs, and there are 256 answers
	// to that question rather than one for each key: replicas() is decided by the key's partition
	// and nothing else. Walking a million keys is otherwise a million copies of the membership and
	// a million hashes of it.
	//
	// One of these is a moment, and it is meant to be short lived: it is built for a single walk
	// and thrown away after it, so a membership that changes is seen by the next walk. Nothing
	// here decides whether giving a record up is safe — the node that owns it is asked that, and
	// asked afresh.
	class placements
	{
		const cluster &nodes;

		std::array<std::optional<placement>, partition_count> known;

	public:
		explicit placements(const cluster &nodes);

		const placement &of(const std::string &key);
	};

	std::optional<router::response> refusal(const std::vector<router::response> &answers);
}

#endif
