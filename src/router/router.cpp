#include "router.h"

router::router::router(repository::repository &repo):
    repository(repo)
{
}

void router::router::get(const std::string &route)
{
	route.~basic_string();
}

void router::router::post(const std::string &route, const std::string &body)
{
    if (route == "table")
    {
	    repository.create_table(body);
    }
}
