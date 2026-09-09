#ifndef ROUTER_RESPONSE_H
#define ROUTER_RESPONSE_H

#include <cstddef>
#include <string>

#include <boost/json.hpp>
#include <boost/beast/http.hpp>

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

	response text_response(boost::beast::http::status status, const std::string &text);

	response empty_response(boost::beast::http::status status);

	// A file of records, and what the node that asked for it needs to ask for the next one.
	response file_response(const std::string &file, size_t records, const std::string &next);

	response head_response(boost::beast::http::status status, const std::string &content_type, size_t length);

	std::string response_body(const response &response);
}

#endif
