#include <boost/json.hpp>

#include "log.h"
#include "router/api_error.h"
#include "url/url.h"
#include "cluster.h"
#include "forwarder.h"

namespace
{
	std::string target(const std::string &node, const router::request &request)
	{
		std::string url = node;

		// The path travels encoded segment by segment, which is how it arrived: a key holding a
		// slash is one segment there and one segment here.
		for (size_t i = 0; i < request.path.size(); i++)
		{
			url += "/" + url::encode(request.path[i]);
		}

		if (request.path.empty())
		{
			url += "/";
		}

		if (!request.query.empty())
		{
			url += "?" + request.query;
		}

		return url;
	}

	// A HEAD travels as a GET, because the node that owns the key is the only one that can say
	// how large the value is, and the session this answer belongs to strips the body itself.
	std::string method_of(const router::request &request)
	{
		if (request.method == boost::beast::http::verb::head)
		{
			return "GET";
		}

		return std::string(boost::beast::http::to_string(request.method));
	}

	router::response to_response(const std::string &node, const http::response &answer)
	{
		if (!answer.is_valid)
		{
			return router::error_response(
				"storage_error", "Node \"" + node + "\" did not answer: " + answer.message + ".");
		}

		boost::beast::http::status status = static_cast<boost::beast::http::status>(answer.status);

		if (answer.body.empty())
		{
			return router::empty_response(status);
		}

		if (answer.content_type.find(router::json_content_type) == std::string::npos)
		{
			return router::text_response(status, answer.body);
		}

		boost::system::error_code error;
		boost::json::value value = boost::json::parse(answer.body, error);

		if (error || !value.is_object())
		{
			return router::error_response(
				"storage_error", "Node \"" + node + "\" answered with something that is not a document.");
		}

		return router::json_response(status, value.as_object());
	}
}

router::response cluster::forward(
	const http::client &http,
	const std::string &node,
	const router::request &request)
{
	std::vector<std::string> headers { std::string(forwarded_header) + ": true" };

	// A write the leader ordered carries the term it ordered it in, and a request no leader
	// ordered carries none at all.
	if (request.term != 0)
	{
		headers.push_back(std::string(term_header) + ": " + std::to_string(request.term));
	}

	http::request forwarded { method_of(request), target(node, request), request.body, headers };

	DEBUG("Forwarding " + forwarded.method + " " + forwarded.url + ".");

	return to_response(node, http.send(forwarded));
}
