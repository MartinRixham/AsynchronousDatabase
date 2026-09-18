#include <algorithm>
#include <expected>
#include <iterator>
#include <utility>

#include <boost/json.hpp>
#include <boost/lexical_cast/try_lexical_convert.hpp>

#include "log.h"
#include "router/api_error.h"
#include "url/url.h"
#include "http_forwarder.h"

namespace
{
	std::string target(const std::string &node, const router::request &request)
	{
		std::string url = node;

		for (const auto &segment : request.path)
		{
			url += "/" + url::encode(segment);
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
			headers.push_back(std::string(cluster::count_header) + ": " + std::to_string(request.count));
		}

		return headers;
	}

	// What a file of records said beside its bytes. A node that answered anything else says
	// nothing here, which is a transfer of no records with nowhere to resume.
	router::transfer transfer_of(const http::response &answer)
	{
		router::transfer file;

		boost::conversion::try_lexical_convert(http::header_of(answer, router::records_header), file.records);

		file.next = http::header_of(answer, router::next_header);

		return file;
	}

	// The body is taken out of the answer rather than copied, being a file of up to the whole
	// budget of a walk.
	router::response body_of(const std::string &node, http::response &answer, bool head)
	{
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
			return router::text_response(status, std::move(answer.body));
		}

		boost::system::error_code error;
		boost::json::value value = boost::json::parse(answer.body, error);

		if (error || !value.is_object())
		{
			return router::error_response(
				error::code::storage_error,
				"Node \"" + node + "\" answered with something that is not a document.");
		}

		return router::json_response(status, value.as_object());
	}

	router::response to_response(const std::string &node, std::expected<http::response, std::string> answer, bool head)
	{
		if (!answer)
		{
			return router::error_response(
				error::code::storage_error,
				"Node \"" + node + "\" did not answer: " + answer.error() + ".");
		}

		router::response response = body_of(node, *answer, head);

		response.file = transfer_of(*answer);

		return response;
	}

	http::request forwarded_to(const std::string &node, const router::request &request)
	{
		http::request forwarded { method_of(request), target(node, request), request.body, headers_of(request) };

		DEBUG("Forwarding " + forwarded.method + " " + forwarded.url + ".");

		return forwarded;
	}

	std::vector<http::request> forwarded_to_all(const std::vector<std::string> &nodes, const router::request &request)
	{
		std::vector<std::string> headers = headers_of(request);
		std::vector<http::request> forwarded;

		for (const auto &destination : nodes)
		{
			forwarded.push_back(
				http::request { method_of(request), target(destination, request), request.body, headers });

			DEBUG("Forwarding " + forwarded.back().method + " " + forwarded.back().url + ".");
		}

		return forwarded;
	}

	// One answer for each node asked, whatever the client made of them, so that the node a
	// refusal belongs to is the node it is reported against.
	std::vector<router::response> responses_of(
		const std::vector<std::string> &nodes,
		std::vector<std::expected<http::response, std::string>> answers,
		const router::request &request)
	{
		std::vector<router::response> responses;

		for (size_t i = 0; i < nodes.size() && i < answers.size(); i++)
		{
			responses.push_back(to_response(nodes[i], std::move(answers[i]), is_head(request)));
		}

		return responses;
	}
}

cluster::http_forwarder::http_forwarder(const http::client &http, long timeout):
	http_client(http),
	timeout_seconds(timeout)
{
}

router::response cluster::http_forwarder::forward(const std::string &node, const router::request &request) const
{
	http::request forwarded = forwarded_to(node, request);

	return to_response(node, http_client.send(forwarded, timeout_seconds), is_head(request));
}

boost::asio::awaitable<router::response> cluster::http_forwarder::async_forward(
	const std::string &node,
	const router::request &request) const
{
	http::request forwarded = forwarded_to(node, request);
	std::expected<http::response, std::string> answer = co_await http_client.async_send(forwarded, timeout_seconds);

	co_return to_response(node, std::move(answer), is_head(request));
}

std::vector<router::response> cluster::http_forwarder::forward_each(const std::vector<enquiry> &enquiries) const
{
	std::vector<http::request> forwarded;

	std::ranges::transform(
		enquiries,
		std::back_inserter(forwarded),
		[](const enquiry &asked) { return forwarded_to(asked.node, asked.request); });

	std::vector<std::expected<http::response, std::string>> answers = http_client.send_all(forwarded, timeout_seconds);
	std::vector<router::response> responses;

	for (size_t i = 0; i < enquiries.size() && i < answers.size(); i++)
	{
		responses.push_back(to_response(enquiries[i].node, std::move(answers[i]), is_head(enquiries[i].request)));
	}

	return responses;
}

std::vector<router::response> cluster::http_forwarder::forward_all(
	const std::vector<std::string> &nodes,
	const router::request &request) const
{
	std::vector<http::request> forwarded = forwarded_to_all(nodes, request);

	return responses_of(nodes, http_client.send_all(forwarded, timeout_seconds), request);
}
