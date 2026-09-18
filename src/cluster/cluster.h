#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "member.h"
#include "partition.h"

namespace cluster
{
	// The key the tables are led by. A table is held by every node rather than by the copies of a
	// partition, so it has no key of its own to hash: one constant is what gives every create and
	// delete of a table the same leader, and one leader is what orders two creates of one name
	// against each other and a create against the drop of a table it depends on. Ordering them is
	// all it does — the operation still goes to every node, because every node holds every table.
	constexpr char table_key[] = "/table";

	struct placement
	{
		bool local = true;

		std::vector<std::string> nodes;
	};

	// What this node's last pass at etcd found. It is this node's own place in the membership and
	// not the cluster's view of etcd: every node reads it for itself, and a node that cannot reach
	// it goes on serving what it holds with a membership that stands still.
	struct etcd_registration
	{
		// Whether the instance was told an etcd at all. One that was not owns the whole keyspace
		// and asks nobody, so neither field below says anything about it.
		bool configured = false;

		// Whether this node holds the lease its registration is written on. It is what says etcd
		// answered: the membership reads back naming this node either way, because a node that
		// etcd did not name is added to what it read.
		bool held = false;

		// The member the next call goes to, which is whichever one last answered.
		std::string endpoint;

		// How many times this node has written its registration. Writing it again is a lease that
		// ran out, so the other nodes may have dropped this one and taken writes it never saw — a
		// change the membership read back afterwards need not show.
		size_t registrations = 0;
	};

	struct leadership
	{
		bool known = false;

		bool local = false;

		std::string node;

		int64_t term = 0;
	};

	// The seam over the other instances, in the way that repository::repository is the seam over
	// the store. A key belongs to one node in each zone, and this is what says which ones: getting
	// a request to one of them is cluster::forwarder's, so that a node can be routed for without
	// being asked and asked without being routed for.
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
		[[nodiscard]] virtual bool discover() = 0;

		virtual void stop() = 0;

		virtual std::vector<member> members() const = 0;

		virtual placement replicas(const std::string &key) const = 0;

		// Where the copies of a partition are, which is replicas() asked of a partition rather
		// than of a key. A scan names a partition and has no key to ask about: every key it could
		// name is in that partition, and the copies of a partition are the same nodes for all of
		// them.
		virtual placement copies_of(size_t partition) const = 0;

		// Every partition this node holds a copy of. replicas() answers the same question of one
		// key, and a pass that moves records has no key to ask about: what it asks another node
		// for is its share, and a share is a set of partitions.
		virtual partition_set holdings() const = 0;

		// How many times the membership this node routes by has been replaced. A pass reads it
		// before holdings(), and hands it back with what it filled.
		virtual uint64_t generation() const = 0;

		// The partitions this node's store holds the whole of, which are the only ones a miss is
		// absence rather than a record never received. A partition is vouched for only if this
		// node has held it without a break since the generation the filling began at, and stops
		// being vouched for the moment the membership takes it away: a partition regained is one
		// it missed the writes of while another node held it.
		virtual void vouch(const partition_set &partitions, uint64_t since) = 0;

		virtual partition_set vouched() const = 0;

		// Which nodes to ask for the records of these partitions, and which of them to ask each
		// node for. A pass filling a share asks the nodes that hold it rather than every node of
		// every zone: one node of a zone holds that zone's copy of a partition, so asking the
		// whole zone is asking every node of it for a share one of them has.
		virtual std::map<std::string, partition_set> holders(const partition_set &partitions) const = 0;

		// The same question of one zone rather than of the membership, which is what a rebuild
		// asks: it fills an empty store from one zone's copy and moves on to the next zone when
		// that one comes up short, so what it needs is the nodes of the zone it is reading.
		virtual std::map<std::string, partition_set> holders_in(
			const partition_set &partitions,
			const std::vector<std::string> &zone) const = 0;

		virtual std::vector<std::string> peers() const = 0;

		virtual std::vector<std::vector<std::string>> zones() const = 0;

		// Not const: a partition nothing claims is claimed here by the node the membership names.
		[[nodiscard]] virtual std::optional<leadership> leader(const std::string &key) = 0;

		virtual size_t leads() const = 0;

		// Whether nothing should be sent to this node: it cannot order a write, and has not been
		// able to for a lease, by which time the rest of the cluster has dropped it. A node that
		// refuses every write and answers every health check is one the load balancer goes on
		// choosing, so the refusal is reported where the load balancer can read it.
		virtual bool is_unled() const = 0;

		// Whether a read here would be answered wrongly: this node has no membership but itself,
		// so it takes itself to hold every key and holds only its share. A membership etcd has
		// stopped answering for is not that, because it is kept as it was last read.
		virtual bool is_alone() const = 0;

		// Where the membership is read from and whether this node is still in it. A membership
		// handed in rather than registered anywhere answers a registration that was never
		// configured.
		virtual etcd_registration registration() const = 0;

		[[nodiscard]] virtual bool accept(const std::string &key, int64_t term) = 0;

		// The terms the store says this node applied — before the process started, or in a file a
		// pass has just taken in — which accept() refuses anything older than as it does the terms
		// applied since. A restart forgets what is held in memory, and a record handed over is fenced
		// by the term it was written in wherever it lands.
		virtual void restore_terms(const partition_terms &applied) = 0;
	};
}
