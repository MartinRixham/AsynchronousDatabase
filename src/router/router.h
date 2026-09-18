#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "api_error.h"
#include "record/record.h"
#include "scan/scan.h"
#include "request.h"
#include "response.h"
#include "cluster/cluster.h"
#include "cluster/forwarder.h"
#include "repository/repository.h"

namespace router
{
	class router
	{
		repository::repository &repository;

		cluster::cluster &nodes;

		// How a request reaches one of them. Where the key lives and how to get there are two
		// questions, and the router asks each of them of the seam that answers it.
		const cluster::forwarder &forwarding;

		// There are far more stripes than the pool has threads because a collision is not cheap:
		// a stripe is held across the fan out, so the second key waits a round trip on the first.
		static constexpr size_t write_stripes = 16384;

		// A cache line each. Several mutexes to a line would have two threads holding *different*
		// stripes writing the same line for the whole of both fan outs, which is the contention
		// the striping is there to avoid. Written out rather than taken from
		// hardware_destructive_interference_size, which GCC warns about using across a boundary.
		static constexpr size_t cache_line = 64;

		struct alignas(cache_line) write_stripe
		{
			std::mutex lock;
		};

		std::array<write_stripe, write_stripes> write_locks;

		std::atomic<bool> draining = false;

	public:
		router(repository::repository &repo, cluster::cluster &nodes, const cluster::forwarder &forwarding);

		response route(const request &request);

		// Whether this node is on its way out. It serves everything as normal and fails its health
		// check, so that a load balancer has stopped choosing it by the time it stops answering.
		void is_draining(bool draining) noexcept;

		bool is_draining() const noexcept;

	private:
		response route_tables(const request &request);

		response route_table(const request &request, const std::string &name);

		response route_range(const request &request, const std::string &name);

		response route_file(const request &request, const std::string &name);

		response route_split(const request &request, const std::string &name);

		response route_record(
			const request &request,
			const std::string &name,
			const std::string &partition,
			const std::string &sort);

		// The lock that orders writes to this key. Only the leader of the key's partition takes
		// it: a copy applies the writes one leader sends it, in the order it is sent them.
		std::mutex &write_lock(const std::string &key);

		// Where the copies of the key are, for a request that has not already worked it out. A
		// forwarded request is served where it lands and asks nothing of the membership, so the
		// leader of a forwarded write is the one caller that has to ask.
		cluster::placement replicas_of(const request &request, const cluster::placement &known, const std::string &key);

		response write_record(
			const request &request,
			const std::string &name,
			const record::record &record,
			const cluster::placement &where);

		response read_record(const request &request, const std::vector<std::string> &replicas);

		// Whether any partition this node holds is one its store is not known to hold the whole
		// of. A table is no partition's, so a table this node does not have is unknown to it while
		// any of them is.
		bool is_incomplete() const;

		// Whether this node holds the partition and cannot vouch for it. One it does not hold is
		// answered out of what its store has, because a forwarded request is served where it lands.
		bool is_short_of(size_t partition) const;

		// Whether this node carries a schema operation out, and what it does with it once it has.
		// A table is not a record of any partition, so what orders one is the leader of
		// cluster::table_key: the same two hops a record write takes, and the same term fencing
		// the copies apply.
		struct ordering
		{
			// Engaged when the answer is settled without this node carrying anything out: the
			// leader's answer to a request forwarded to it, or the refusal of one that cannot be
			// ordered anywhere.
			std::optional<response> answer;

			// The nodes the operation is carried on to. Every other node when this node is the
			// one ordering it, and none when the node that ordered it sent it here.
			std::vector<std::string> peers;

			int64_t term = 0;

			// Whether the node that ordered this sent it here, which is what tells the two hops
			// a schema operation takes apart: a request forwarded *to* the leader carries no term
			// and is the client's own, and one carried *from* it is an order to apply. Both arrive
			// marked forwarded, so the flag alone cannot say which.
			bool carried = false;
		};

		ordering order_schema(const request &request);

		record::version schema_stamp(const request &request, const ordering &order) const;

		response create_table(const request &request, const std::string &name);

		response delete_table(const request &request, const std::string &name);

		response scan_records(const request &request, const std::string &name);

		response answer_page(const request &request, const std::string &name);

		std::set<std::string> table_names() const;
	};
}
