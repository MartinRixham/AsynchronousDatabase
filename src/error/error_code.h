#pragma once

#include <string>

namespace error
{
	// The documented error codes. A code is what a client branches on, so each one has a name on
	// the wire and a status, and a code missing from either switch is a build that fails.
	enum class code
	{
		table_not_found,
		table_exists,
		invalid_table_name,
		invalid_body,
		dependency_not_found,
		invalid_key_encoding,
		key_too_large,
		value_too_large,
		invalid_range,
		invalid_cursor,
		invalid_partition,
		invalid_partitions,
		write_stalled,
		no_leader,
		node_incomplete,
		node_alone,
		stale_leader,
		storage_error,
		not_found,
		invalid_path,
		method_not_allowed
	};

	std::string name(code error);
}
