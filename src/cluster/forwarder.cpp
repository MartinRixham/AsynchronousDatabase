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

	std::string method_of(const router::request &request)
	{
		return std::string(boost::beast::http::to_string(request.method));
	}

	bool is_head(const router::request &request)
	{
		return request.method == boost::beast::http::verb::head;
	}

	std::vector<std::string> headers_of(const router::request &request)
	{
		std::vector<std::string> headers { std::string(cluster::forwarded_header) + ": true" };

		if (request.term != 0)
		{
			headers.push_back(std::string(cluster::term_header) + ": " + std::to_string(request.term));
		}

		return headers;
	}

	router::response to_response(const std::string &node, const http::response &answer, bool head)
	{
		if (!answer.is_valid)
		{
			return router::error_response(
				"storage_error", "Node \"" + node + "\" did not answer: " + answer.message + ".");
		}

		boost::beast::http::status status = static_cast<boost::beast::http::status>(answer.status);

		if (head)
		{
			return router::head_response(status, answer.content_type, static_cast<size_t>(answer.content_length));
		}

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
	http::request forwarded { method_of(request), target(node, request), request.body, headers_of(request) };

	DEBUG("Forwarding " + forwarded.method + " " + forwarded.url + ".");

	return to_response(node, http.send(forwarded), is_head(request));
}

std::vector<router::response> cluster::forward_all(
	const http::client &http,
	const std::vector<std::string> &nodes,
	const router::request &request)
{
	std::vector<std::string> headers = headers_of(request);
	std::vector<http::request> forwarded;

	for (size_t i = 0; i < nodes.size(); i++)
	{
		forwarded.push_back(
			http::request { method_of(request), target(nodes[i], request), request.body, headers });

		DEBUG("Forwarding " + forwarded.back().method + " " + forwarded.back().url + ".");
	}

	std::vector<http::response> answers = http.send_all(forwarded);
	std::vector<router::response> responses;

	// One answer for each node asked, whatever the client made of them, so that the node a
	// refusal belongs to is the node it is reported against.
	for (size_t i = 0; i < nodes.size() && i < answers.size(); i++)
	{
		responses.push_back(to_response(nodes[i], answers[i], is_head(request)));
	}

	return responses;
}
