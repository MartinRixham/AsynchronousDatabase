#pragma once

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/span_body.hpp>

#include "http_client.h"
#include "location.h"

namespace http
{
	// A request as Beast sends it. The body is the caller's and is not copied into it, which is
	// why the requests of a fan out have to outlive the call that carries them.
	using message = boost::beast::http::request<boost::beast::http::span_body<const char>>;

	message message_of(const location &where, const request &sending);
}
