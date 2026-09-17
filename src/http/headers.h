#pragma once

#include <cstdint>

#include <boost/beast/http/message.hpp>

#include "http_client.h"

namespace http
{
	// Beast bounds the headers of an answer at eight kibibytes of its own, and an answer names the
	// key a walk is to resume at — base64 of a key that may be four kibibytes by itself.
	constexpr std::uint32_t header_bound = 64 * 1024;

	// What an answer says before its body, read into the response the caller holds. The body is
	// not read here: one caller keeps it whole and the other hands it on as it arrives.
	void take_headers(const boost::beast::http::response_header<> &received, response &answer);
}
