#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>

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

		// What a schema operation is ordered under, so that a create is validated against the tables
		// as they stand when it is carried out.
		std::mutex schema_lock;

		std::atomic<bool> draining = false;

	public:
		router(repository::repository &repo, cluster::cluster &nodes, const cluster::forwarder &forwarding);

		// Worked out as it is called, and awaited on the executor of the connection it arrived on for
		// what it waits on. A request for a record asks the other nodes without holding a thread;
		// everything else is answered on the thread serving it, the nodes it asks included. The
		// request has to outlive the wait.
		boost::asio::awaitable<response> route(const request &request);

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

		boost::asio::awaitable<response> route_record(
			const request &request,
			const std::string &name,
			const std::string &partition,
			const std::string &sort);

		// A write the leader ordered, which the copy it was carried to writes and sends nowhere.
		response apply_write(
			const request &request,
			const std::string &name,
			const std::string &partition,
			const std::string &sort);

		// A client's write: sent to the leader of its partition, or stamped and carried to every
		// copy by this node when it is the leader.
		boost::asio::awaitable<response> order_write(
			const request &request,
			const std::string &name,
			record::record record);

		response write_record(
			const request &request,
			const std::string &name,
			const record::record &record,
			const cluster::placement &where);

		// What waits on another node takes what it needs by value, the call that started it having
		// returned before it runs. GCC reports a coroutine frame much over a kibibyte as a
		// mismatched delete, which is why the deciding is done before either of them is started.
		boost::asio::awaitable<response> forward_to_leader(const request &request, cluster::leadership lead);

		// The answer of the first copy that answered, passing over one that did not.
		boost::asio::awaitable<response> read_record(const request &request, cluster::placement where);

		// The same for a scan, asked on the thread serving it.
		response read_copies(const request &request, const std::vector<std::string> &replicas);

		// Whether any partition this node holds is one its store is not known to hold the whole
		// of. A table is no partition's, so a table this node does not have is unknown to it while
		// any of them is.
		bool is_incomplete() const;

		// Whether this node holds the partition and cannot vouch for it. One it does not hold is
		// answered out of what its store has, because a forwarded request is served where it lands.
		bool is_short_of(size_t partition) const;

		// The answer for a table this node does not have.
		response missing_table(const std::string &name) const;

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
