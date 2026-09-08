#ifndef ROUTER_RESPONSE_H
#define ROUTER_RESPONSE_H

#include <string>

#include <boost/json.hpp>
#include <boost/beast/http.hpp>

namespace router
{
	constexpr char json_content_type[] = "application/json";

	// A value travels as the body and nothing else: there is no envelope to unwrap.
	constexpr char text_content_type[] = "text/plain; charset=utf-8";

	struct response
	{
		boost::beast::http::status status = boost::beast::http::status::ok;

		// Empty when the response has no body at all, which is how a missing key is told from an
		// empty value.
		std::string content_type;

		boost::json::object json;

		std::string text;

		// The length of a body that is not here: what another node said about the value it left
		// out of a HEAD. Nothing else answers the size of a body it does not carry.
		size_t length = 0;
	};

	response json_response(boost::beast::http::status status, const boost::json::object &json);

	response text_response(boost::beast::http::status status, const std::string &text);

	response empty_response(boost::beast::http::status status);

	// The headers of a body and none of the body, which is what a HEAD another node answered
	// carries back.
	response head_response(boost::beast::http::status status, const std::string &content_type, size_t length);

	std::string response_body(const response &response);
}

#endif
