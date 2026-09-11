#ifndef ROUTER_ROUTER_H
#define ROUTER_ROUTER_H

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
#include "repository/repository.h"

namespace router
{
	class router
	{
		repository::repository &repository;

		cluster::cluster &nodes;

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

		// Read by every session and written once by the thread that filled the store, so it is an
		// atomic rather than a field a reader happens to see.
		std::atomic<bool> incomplete = false;

	public:
		router(repository::repository &repo, cluster::cluster &nodes);

		response route(const request &request);

		// Whether this node holds less than it owns, which is a rebuild that did not read the
		// whole of its share. A miss it reports is then absence it cannot vouch for, so a read is
		// answered node_incomplete and the node that asked tries the next copy instead of
		// believing it. Cleared by a reconcile pass that settles, which is this node having
		// fetched everything it owns and holds nothing for.
		void is_incomplete(bool incomplete);

		bool is_incomplete() const;

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

		response create_table(const request &request, const std::string &name);

		response delete_table(const request &request, const std::string &name);

		response scan_records(const request &request, const std::string &name);

		// What one zone said to a scan: the page its nodes answered with, or the answer that failed.
		struct zone_answer
		{
			std::optional<response> failure;

			scan::page page;
		};

		zone_answer scan_zone(const request &request, const scan::range &range, const std::vector<std::string> &zone);

		std::set<std::string> table_names() const;

	};
}

#endif
