#ifndef ROUTER_ROUTER_H
#define ROUTER_ROUTER_H

#include <set>
#include <string>

#include "api_error.h"
#include "request.h"
#include "response.h"
#include "repository/repository.h"

namespace router
{
	class router
	{
		repository::repository &repository;

	public:
		explicit router(repository::repository &repo);

		response route(const request &request);

	private:
		response route_tables(const request &request);

		response route_table(const request &request, const std::string &name);

		response route_range(const request &request, const std::string &name);

		response route_record(const request &request, const std::string &name, const std::string &key);

		response create_table(const std::string &name, const std::string &body);

		response scan_records(const std::string &name, const std::string &query);

		response delete_records(const std::string &name, const std::string &query);

		std::set<std::string> table_names() const;

	};
}

#endif
