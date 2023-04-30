#ifndef ROUTER_ROUTER_H
#define ROUTER_ROUTER_H

#include <boost/json.hpp>
#include <boost/beast/http.hpp>

#include "repository/rocksdb_repository.h"

namespace router
{
	struct response
	{
		boost::beast::http::status status;
		
		boost::json::object body;
	};

	class router
	{
        repository::repository &repository;

	public:
		explicit router(repository::repository &repo);

		response get(const std::string &route);

		response post(const std::string &route, const boost::json::object &body);
	};
}

#endif
