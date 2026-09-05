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
	// A request that has already been sent to the node that owns its key. That node serves it
	// where it stands, so that two nodes that disagree about the membership for a moment cannot
	// bounce a request between them for ever.
	constexpr char forwarded_header[] = "X-Asyncdb-Forwarded";

	// The term the leader of a partition ordered a write in. A copy refuses anything older than
	// the newest term it has applied, which is what stops a leader that has been replaced.
	constexpr char term_header[] = "X-Asyncdb-Term";

	// Where the copies of a key are: whether this node holds one itself, and the other nodes that
	// hold one, in the order this node should ask them — its own zone first, because the copy in
	// this node's zone is the near one.
	struct placement
	{
		// True when this node holds the key, which is what an instance standing alone answers for
		// every key.
		bool local = true;

		std::vector<std::string> nodes;
	};

	// Who orders the writes to a partition, and in which term. One node leads a partition at a
	// time: it applies a write itself and sends it to the copies, so that two clients writing one
	// key are ordered by one node rather than racing at each copy.
	//
	// The term is etcd's revision of the claim, so a later leader of a partition always has a
	// higher term than the leader before it, and a write carrying an older one is refused by the
	// copies. That is what stops a leader that has lost its lease, and does not know it yet, from
	// writing behind the leader that replaced it.
	struct leadership
	{
		// False when no node holds the partition — nothing has claimed it yet, or etcd cannot be
		// read. Writes wait for a leader rather than going around one.
		bool known = false;

		// True when this node is the leader, which is where the order is actually decided.
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
		// Every node including this one, in name order. Empty when this instance stands alone.
		virtual std::vector<member> members() const = 0;

		// Every node holding a copy of the key.
		virtual placement replicas(const std::string &key) const = 0;

		// Every node but this one, which is what an operation on every node is carried out over.
		virtual std::vector<std::string> peers() const = 0;

		// The zones a scan is asked of, as the nodes of each and without this node, in the order
		// to ask them: a zone holds a copy of the whole keyspace, so the first of them answers a
		// scan whole, and the rest are what to fall back on when a node of it does not answer.
		// Empty when this instance stands alone.
		virtual std::vector<std::vector<std::string>> zones() const = 0;

		// The node that orders writes to this key's partition. Nothing at all when there is nothing
		// to order — one instance standing alone owns every key and races with nobody.
		virtual std::optional<leadership> leader(const std::string &key) const = 0;

		// How many partitions this node leads, which is what says an election has settled. Nothing
		// at all when there is no leadership to have.
		virtual size_t leads() const = 0;

		// Whether a write of this key, ordered in that term, may be applied here — and remembers
		// the term, so that anything older is refused from then on. A term of nothing is a write
		// that no leader ordered, which is a cluster with no leadership at all.
		virtual bool accept(const std::string &key, int64_t term) = 0;

		// Sends the request to the node named and returns its answer as this node's own. The node
		// serves it where it stands rather than forwarding it again.
		virtual router::response send(const std::string &node, const router::request &request) const = 0;
	};
}

#endif
