#include <map>

#include "api_error.h"

namespace
{
	const std::map<std::string, boost::beast::http::status> statuses {
		{ "table_not_found", boost::beast::http::status::not_found },
		{ "table_exists", boost::beast::http::status::conflict },
		{ "invalid_table_name", boost::beast::http::status::bad_request },
		{ "invalid_body", boost::beast::http::status::bad_request },
		{ "dependency_not_found", boost::beast::http::status::bad_request },
		{ "invalid_key_encoding", boost::beast::http::status::bad_request },
		{ "key_too_large", boost::beast::http::status::payload_too_large },
		{ "value_too_large", boost::beast::http::status::payload_too_large },
		{ "invalid_range", boost::beast::http::status::bad_request },
		{ "invalid_cursor", boost::beast::http::status::bad_request },
		{ "write_stalled", boost::beast::http::status::service_unavailable },
		{ "no_leader", boost::beast::http::status::service_unavailable },
		{ "stale_leader", boost::beast::http::status::conflict },
		{ "storage_error", boost::beast::http::status::internal_server_error },
		{ "not_found", boost::beast::http::status::not_found },
		{ "invalid_path", boost::beast::http::status::bad_request },
		{ "method_not_allowed", boost::beast::http::status::method_not_allowed }
	};
}

router::response router::error_response(const std::string &code, const std::string &message)
{
	boost::json::object error { { "code", code }, { "message", message } };

	return json_response(error_status(code), boost::json::object { { "error", error } });
}

boost::beast::http::status router::error_status(const std::string &code)
{
	std::map<std::string, boost::beast::http::status>::const_iterator status = statuses.find(code);

	if (status == statuses.end())
	{
		return boost::beast::http::status::internal_server_error;
	}

	return status->second;
}
