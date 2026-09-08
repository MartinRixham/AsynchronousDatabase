#ifndef ROUTER_RESPONSE_H
#define ROUTER_RESPONSE_H

#include <string>

#include <boost/json.hpp>
#include <boost/beast/http.hpp>

namespace router
{
	constexpr char json_content_type[] = "application/json";

	constexpr char text_content_type[] = "text/plain; charset=utf-8";

	struct response
	{
		boost::beast::http::status status = boost::beast::http::status::ok;

		std::string content_type;

		boost::json::object json;

		std::string text;

		size_t length = 0;
	};

	response json_response(boost::beast::http::status status, const boost::json::object &json);

	response text_response(boost::beast::http::status status, const std::string &text);

	response empty_response(boost::beast::http::status status);

	response head_response(boost::beast::http::status status, const std::string &content_type, size_t length);

	std::string response_body(const response &response);
}

#endif
