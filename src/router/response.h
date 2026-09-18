#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include <boost/json.hpp>
#include <boost/beast/http.hpp>

#include "repository/scratch_file.h"

namespace router
{
	constexpr char json_content_type[] = "application/json";

	constexpr char text_content_type[] = "text/plain; charset=utf-8";

	constexpr char file_content_type[] = "application/octet-stream";

	// What a file of records answers beside its bytes. The body is a file and not a document, so
	// there is nowhere in it to put either: they travel as headers of their own.
	constexpr char records_header[] = "X-Asyncdb-Records";

	constexpr char next_header[] = "X-Asyncdb-Next";

	struct transfer
	{
		size_t records = 0;

		// The key the file after this one resumes at, base64 so that a header carries a key of any
		// bytes at all. Empty is a walk that reached the end of the table.
		std::string next;

		// The file itself where this node is the one sending it, which is sent from disk rather
		// than held. A file that came over the network is the text of the response instead.
		std::shared_ptr<const repository::scratch_file> sent;
	};

	struct response
	{
		boost::beast::http::status status = boost::beast::http::status::ok;

		std::string content_type;

		boost::json::object json;

		std::string text;

		size_t length = 0;

		transfer file;
	};

	response json_response(boost::beast::http::status status, const boost::json::object &json);

	response text_response(boost::beast::http::status status, std::string text);

	response empty_response(boost::beast::http::status status);

	// A file of records, and what the node that asked for it needs to ask for the next one.
	response file_response(
		std::shared_ptr<const repository::scratch_file> file,
		size_t records,
		const std::string &next);

	response head_response(boost::beast::http::status status, const std::string &content_type, size_t length);

	std::string response_body(const response &response);
}
