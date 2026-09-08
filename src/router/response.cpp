#include "response.h"

router::response router::json_response(boost::beast::http::status status, const boost::json::object &json)
{
	return { status, json_content_type, json, "", 0 };
}

router::response router::text_response(boost::beast::http::status status, const std::string &text)
{
	return { status, text_content_type, boost::json::object(), text, 0 };
}

router::response router::empty_response(boost::beast::http::status status)
{
	return { status, "", boost::json::object(), "", 0 };
}

router::response router::head_response(
	boost::beast::http::status status,
	const std::string &content_type,
	size_t length)
{
	return { status, content_type, boost::json::object(), "", length };
}

std::string router::response_body(const response &response)
{
	// A body another node left out of a HEAD is one there is nothing to write for, whatever its
	// headers say it would have been.
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
