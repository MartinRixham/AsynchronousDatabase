#pragma once

#include <string>

#include "error/error_code.h"
#include "response.h"

namespace router
{
	// Every error carries the same body, and the code, not the message and not the status, is
	// what a client should branch on.
	response error_response(error::code code, const std::string &message);

	boost::beast::http::status error_status(error::code code);
}
