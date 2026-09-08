#include "router/api_error.h"
#include "standalone.h"

std::vector<cluster::member> cluster::standalone::members() const
{
	return std::vector<member>();
}

cluster::placement cluster::standalone::replicas(const std::string &) const
{
	return placement();
}

std::vector<std::string> cluster::standalone::peers() const
{
	return std::vector<std::string>();
}

std::vector<std::vector<std::string>> cluster::standalone::zones() const
{
	return std::vector<std::vector<std::string>>();
}

std::optional<cluster::leadership> cluster::standalone::leader(const std::string &) const
{
	return std::nullopt;
}

// One instance leads nothing, because there is nothing to order.
size_t cluster::standalone::leads() const
{
	return 0;
}

bool cluster::standalone::accept(const std::string &, int64_t)
{
	return true;
}

// There is no node to send to, so nothing reaches this, and answering rather than throwing keeps
// it a value like every other failure the router turns into a status.
router::response cluster::standalone::send(const std::string &node, const router::request &) const
{
	return router::error_response("storage_error", "There is no node named \"" + node + "\" to answer for.");
}
