#ifndef TRANSFER_TRANSFER_H
#define TRANSFER_TRANSFER_H

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>

#include "cluster/cluster.h"
#include "cluster/partition.h"
#include "repository/repository.h"
#include "progress/patience.h"

namespace transfer
{
	// How many pieces of a table a walk is cut into, and so how many of its files are being asked
	// for at once. One walk on one thread moves a share at the speed of one request at a time —
	// ask, wait, take it in, ask again — with the node it is asking, the network and the store
	// each idle for most of it. Four keeps all three busy without holding more connections to a
	// node that is serving clients besides.
	constexpr size_t default_workers = 4;

	// What a walk does with each file it is handed. **Called from every worker at once**, so what
	// it does has to be safe for that: taking the files of a share in beside each other is half of
	// what the workers are for.
	typedef std::function<void(const std::string &)> taking;

	// One node's share of one table, as the node asking for it names it.
	struct share
	{
		std::string node;

		std::string table;

		cluster::partition_set partitions;

		bool values = true;

		size_t workers = default_workers;

		// How much of the table the whole walk holds in memory at once. It is shared out between
		// the pieces, so a share read in eight pieces costs what one read in one piece does.
		size_t bytes = repository::max_file_bytes;
	};

	// What a walk did: whether every piece was read to the end of the table, and — when it was not
	// — whether that was the node it was asking or the walk's own patience running out. The two are
	// not the same thing to the caller. A rebuild has another zone it can ask instead; a pass that
	// moves records has nothing more to do this time about a node that is not answering, and asks
	// it again on the next one.
	struct outcome
	{
		bool whole = false;

		bool refused = false;
	};

	// Asks a node for its share of a table and hands every file to `take`, over several key ranges
	// at once and asking for the next file of each while the last one is still being taken in.
	outcome walk(
		const cluster::cluster &nodes,
		const share &wanted,
		const std::atomic<bool> &running,
		progress::patience &waiting,
		const taking &take);
}

#endif
