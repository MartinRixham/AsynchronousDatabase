#ifndef CLUSTER_CLUSTER_H
#define CLUSTER_CLUSTER_H

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

	// The count the leader stamped the write with. The term beside it is the other half of the
	// version, so this travels only where that does.
	constexpr char count_header[] = "X-Asyncdb-Count";

	// The key the tables are led by. A table is held by every node rather than by the copies of a
	// partition, so it has no key of its own to hash: one constant is what gives every create and
	// delete of a table the same leader, and one leader is what orders two creates of one name
	// against each other and a create against the drop of a table it depends on. Ordering them is
	// all it does — the operation still goes to every node, because every node holds every table.
	constexpr char table_key[] = "/table";

	// A request and the node it is for, so that several different ones can be asked at once.
	struct enquiry
	{
		std::string node;

		router::request request;
	};

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
		// Joining the cluster, and leaving it again. A membership that was handed in rather than
		// registered anywhere answers both and does nothing.
		virtual void start() = 0;

		// Reads the membership without joining it, so that a node can see what it is about to hold
		// before anything is routed to it. It is what a rebuild runs on: a node that has not
		// registered is nobody's copy, so it can take as long as it needs.
		//
		// False when there was no membership to read, which is an instance standing alone or a
		// cluster a test handed in. A membership that was not read here is not one a rebuild should
		// act on: its nodes were never asked whether they are serving yet.
		virtual bool discover() = 0;

		virtual void stop() = 0;

		virtual std::vector<member> members() const = 0;

		virtual placement replicas(const std::string &key) const = 0;

		// Every partition this node holds a copy of. replicas() answers the same question of one
		// key, and a pass that moves records has no key to ask about: what it asks another node
		// for is its share, and a share is a set of partitions.
		virtual partition_set holdings() const = 0;

		virtual std::vector<std::string> peers() const = 0;

		virtual std::vector<std::vector<std::string>> zones() const = 0;

		virtual std::optional<leadership> leader(const std::string &key) const = 0;

		virtual size_t leads() const = 0;

		// Whether nothing should be sent to this node: it cannot order a write, and has not been
		// able to for a lease, by which time the rest of the cluster has dropped it. A node that
		// refuses every write and answers every health check is one the load balancer goes on
		// choosing, so the refusal is reported where the load balancer can read it.
		virtual bool is_unled() const = 0;

		virtual bool accept(const std::string &key, int64_t term) = 0;

		virtual router::response send(const std::string &node, const router::request &request) const = 0;

		// A cluster that can ask them at once asks them at once. A write is not done until every
		// copy has taken it, and asking one after another holds the thread serving the write for a
		// round trip each — and a thread waiting on another node cannot answer anything else, its
		// health check included.
		virtual std::optional<router::response> send_all(
			const std::vector<std::string> &node_list,
			const router::request &request) const = 0;

		// A different request to each node named, all of them at once, answered one for one and in
		// the order they were given. send_all() is this with everything but a refusal thrown away,
		// which is what a write to the copies of a record wants; a walk reading a share in several
		// pieces wants what each answer carried, and asks for a different piece in each.
		virtual std::vector<router::response> send_each(const std::vector<enquiry> &enquiries) const = 0;
	};

	std::optional<router::response> refusal(const std::vector<router::response> &answers);
}

#endif
