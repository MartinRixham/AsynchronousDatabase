#include "response.h"

router::response router::json_response(boost::beast::http::status status, const boost::json::object &json)
{
	return { status, json_content_type, json, "" };
}

router::response router::text_response(boost::beast::http::status status, const std::string &text)
{
	return { status, text_content_type, boost::json::object(), text };
}

router::response router::empty_response(boost::beast::http::status status)
{
	return { status, "", boost::json::object(), "" };
}

std::string router::response_body(const response &response)
{
	if (response.content_type == json_content_type)
	{
		return boost::json::serialize(response.json);
	}

	return response.text;
}
