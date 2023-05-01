#ifndef ROUTER_ROUTER_H
#define ROUTER_ROUTER_H

#include <boost/json.hpp>
#include <boost/beast/http.hpp>

#include "response.h"
#include "repository/rocksdb_repository.h"

namespace router
{
	class router
	{
        repository::repository &repository;

	public:
		explicit router(repository::repository &repo);

		response get(const std::string &route) const;

		response post(const std::string &route, const boost::json::object &body);
	};
}

#endif
