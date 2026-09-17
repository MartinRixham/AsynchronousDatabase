#include "api_error.h"

router::response router::error_response(error::code code, const std::string &message)
{
	boost::json::object error { { "code", error::name(code) }, { "message", message } };

	return json_response(error_status(code), boost::json::object { { "error", error } });
}

boost::beast::http::status router::error_status(error::code code)
{
	using boost::beast::http::status;

	switch (code)
	{
	case error::code::table_not_found:
	case error::code::not_found:
		return status::not_found;
	case error::code::table_exists:
	case error::code::stale_leader:
		return status::conflict;
	case error::code::invalid_table_name:
	case error::code::invalid_body:
	case error::code::dependency_not_found:
	case error::code::invalid_key_encoding:
	case error::code::invalid_range:
	case error::code::invalid_cursor:
	case error::code::invalid_partition:
	case error::code::invalid_partitions:
	case error::code::invalid_path:
		return status::bad_request;
	case error::code::key_too_large:
	case error::code::value_too_large:
		return status::payload_too_large;
	case error::code::write_stalled:
	case error::code::no_leader:
	case error::code::node_incomplete:
	case error::code::node_alone:
		return status::service_unavailable;
	case error::code::storage_error:
		return status::internal_server_error;
	case error::code::method_not_allowed:
		return status::method_not_allowed;
	}

	return status::internal_server_error;
}
