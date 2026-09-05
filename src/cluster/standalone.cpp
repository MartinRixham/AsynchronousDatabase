#include "router/api_error.h"
#include "standalone.h"

std::vector<cluster::member> cluster::standalone::members() const
{
	return std::vector<member>();
}

// One instance holds the only copy of every key, and holds it here.
cluster::placement cluster::standalone::replicas(const std::string &) const
{
	return placement();
}

std::vector<std::string> cluster::standalone::peers() const
{
	return std::vector<std::string>();
}

// One instance is every zone there is, and it scans its own store.
std::vector<std::vector<std::string>> cluster::standalone::zones() const
{
	return std::vector<std::vector<std::string>>();
}

// There is no node to send to, so nothing reaches this, and answering rather than throwing keeps
// it a value like every other failure the router turns into a status.
router::response cluster::standalone::send(const std::string &node, const router::request &) const
{
	return router::error_response("storage_error", "There is no node named \"" + node + "\" to answer for.");
}
