#include "placements.h"

cluster::placements::placements(const cluster &nodes):
	nodes(nodes)
{
}

const cluster::placement &cluster::placements::of(const std::string &key)
{
	std::optional<placement> &answer = known[partition_of(key)];

	if (!answer)
	{
		answer = nodes.replicas(key);
	}

	return *answer;
}
