#ifndef ROUTER_ROUTER_H
#define ROUTER_ROUTER_H

#include <boost/json.hpp>

#include "repository/rocksdb_repository.h"

namespace router
{
	class router
	{
        repository::repository &repository;

	public:
		explicit router(repository::repository &repo);

		boost::json::object get(const std::string &route);

		boost::json::object post(const std::string &route, const boost::json::object &body);
	};
}

#endif
