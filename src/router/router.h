#ifndef ROUTER_ROUTER_H
#define ROUTER_ROUTER_H

#include <array>
#include <cstddef>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include "api_error.h"
#include "record/record.h"
#include "scan/scan.h"
#include "request.h"
#include "response.h"
#include "cluster/cluster.h"
#include "cluster/standalone.h"
#include "repository/repository.h"

namespace router
{
	class router
	{
		repository::repository &repository;

		// What a router built without a cluster routes against: one node owning every key.
		cluster::standalone alone;

		cluster::cluster &nodes;

		// Keys are striped over a fixed set of locks rather than given one each: a lock lives as
		// long as one write and no longer, so what there has to be enough of is the writes in
		// flight here — which the thread pool bounds — and not the keys in the store. Two keys
		// that collide are two keys written one after the other and nothing worse.
		//
		// There are far more of them than the pool has threads because a collision is not cheap:
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

	public:
		explicit router(repository::repository &repo);

		router(repository::repository &repo, cluster::cluster &nodes);

		response route(const request &request);

	private:
		response route_tables(const request &request);

		response route_table(const request &request, const std::string &name);

		response route_range(const request &request, const std::string &name);

		response route_record(const request &request, const std::string &name, const std::string &key);

		// The lock that orders writes to this key. Only the leader of the key's partition takes
		// it: a copy applies the writes one leader sends it, in the order it is sent them.
		std::mutex &write_lock(const std::string &key);

		// Where the copies of the key are, for a request that has not already worked it out. A
		// forwarded request is served where it lands and asks nothing of the membership, so the
		// leader of a forwarded write is the one caller that has to ask.
		cluster::placement replicas_of(const request &request, const cluster::placement &known, const std::string &key);

		// Writes the record on this node when it holds a copy, and on every other node that holds
		// one. A copy that refuses fails the request.
		response write_record(
			const request &request,
			const std::string &name,
			const record::record &record,
			const cluster::placement &where);

		// Asks the nodes holding a copy of the key in turn, and answers with the first one that
		// answered at all.
		response read_record(const request &request, const std::vector<std::string> &replicas);

		response create_table(const request &request, const std::string &name);

		response delete_table(const request &request, const std::string &name);

		response scan_records(const request &request, const std::string &name);

		// Asks one zone for the same range and adds what it answers to the records this node holds
		// itself. Answers the failure when a node of that zone did not answer, which is a zone to
		// give up on rather than a scan to fail.
		std::optional<response> scan_zone(
			const request &request,
			const scan::range &range,
			const std::vector<std::string> &zone,
			std::vector<record::record> *records,
			bool *has_more);

		response delete_records(const request &request, const std::string &name);

		// Sends the request to every other node and returns the first answer that is an error, or
		// nothing at all when every node agreed. A request that arrived forwarded goes no further.
		std::optional<response> broadcast(const request &request);

		std::set<std::string> table_names() const;

	};
}

#endif
