#include "router/api_error.h"
#include "standalone.h"

std::vector<std::string> cluster::standalone::members() const
{
	return std::vector<std::string>();
}

std::optional<std::string> cluster::standalone::owner(const std::string &) const
{
	return std::nullopt;
}

std::vector<std::string> cluster::standalone::peers() const
{
	return std::vector<std::string>();
}

// There is no node to send to, so nothing reaches this, and answering rather than throwing keeps
// it a value like every other failure the router turns into a status.
router::response cluster::standalone::send(const std::string &node, const router::request &) const
{
	return router::error_response("storage_error", "There is no node named \"" + node + "\" to answer for.");
}
