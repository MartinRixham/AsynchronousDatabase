#ifndef ROUTER_ROUTER_H
#define ROUTER_ROUTER_H

#include "repository/repository.h"

namespace router
{
	class router
	{
        repository::repository &repository;

	public:
		explicit router(repository::repository &repo);

		void get(const std::string &route);

		void post(const std::string &route, const std::string &body);
	};
}

#endif