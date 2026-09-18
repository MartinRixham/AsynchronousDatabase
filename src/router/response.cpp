#include <utility>

#include "response.h"

router::response router::json_response(boost::beast::http::status status, const boost::json::object &json)
{
	return { status, json_content_type, json, "", 0, {} };
}

router::response router::text_response(boost::beast::http::status status, std::string text)
{
	return { status, text_content_type, boost::json::object(), std::move(text), 0, {} };
}

router::response router::empty_response(boost::beast::http::status status)
{
	return { status, "", boost::json::object(), "", 0, {} };
}

router::response router::file_response(
	std::shared_ptr<const repository::scratch_file> file,
	size_t records,
	const std::string &next)
{
	return { boost::beast::http::status::ok,	file_content_type, boost::json::object(), "", 0,
			 { records, next, std::move(file) } };
}

router::response router::head_response(
	boost::beast::http::status status,
	const std::string &content_type,
	size_t length)
{
	return { status, content_type, boost::json::object(), "", length, {} };
}

std::string router::response_body(const response &response)
{
	if (response.length > 0)
	{
		return "";
	}

	if (response.content_type == json_content_type)
	{
		return boost::json::serialize(response.json);
	}

	return response.text;
}
