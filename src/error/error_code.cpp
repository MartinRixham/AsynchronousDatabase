#include "error_code.h"

std::string error::name(code error)
{
	switch (error)
	{
	case code::table_not_found:
		return "table_not_found";
	case code::table_exists:
		return "table_exists";
	case code::invalid_table_name:
		return "invalid_table_name";
	case code::invalid_body:
		return "invalid_body";
	case code::dependency_not_found:
		return "dependency_not_found";
	case code::invalid_key_encoding:
		return "invalid_key_encoding";
	case code::key_too_large:
		return "key_too_large";
	case code::value_too_large:
		return "value_too_large";
	case code::invalid_range:
		return "invalid_range";
	case code::invalid_cursor:
		return "invalid_cursor";
	case code::invalid_partition:
		return "invalid_partition";
	case code::invalid_partitions:
		return "invalid_partitions";
	case code::write_stalled:
		return "write_stalled";
	case code::no_leader:
		return "no_leader";
	case code::node_incomplete:
		return "node_incomplete";
	case code::node_alone:
		return "node_alone";
	case code::stale_leader:
		return "stale_leader";
	case code::storage_error:
		return "storage_error";
	case code::not_found:
		return "not_found";
	case code::invalid_path:
		return "invalid_path";
	case code::method_not_allowed:
		return "method_not_allowed";
	}

	// Only a value cast from outside the enumeration reaches here.
	return "storage_error";
}
