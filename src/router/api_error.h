#ifndef ROUTER_API_ERROR_H
#define ROUTER_API_ERROR_H

#include <string>

#include "response.h"

namespace router
{
	// Every error carries the same body, and the code, not the message and not the status, is
	// what a client should branch on.
	response error_response(const std::string &code, const std::string &message);

	boost::beast::http::status error_status(const std::string &code);
}

#endif
