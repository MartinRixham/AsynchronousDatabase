#ifndef ROUTER_ROUTER_H
#define ROUTER_ROUTER_H

#include <optional>
#include <set>
#include <string>

#include "api_error.h"
#include "record/record.h"
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

	public:
		explicit router(repository::repository &repo);

		router(repository::repository &repo, cluster::cluster &nodes);

		response route(const request &request);

	private:
		response route_tables(const request &request);

		response route_table(const request &request, const std::string &name);

		response route_range(const request &request, const std::string &name);

		response route_record(const request &request, const std::string &name, const std::string &key);

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

		response delete_records(const request &request, const std::string &name);

		// Sends the request to every other node and returns the first answer that is an error, or
		// nothing at all when every node agreed. A request that arrived forwarded goes no further.
		std::optional<response> broadcast(const request &request);

		std::set<std::string> table_names() const;

	};
}

#endif
