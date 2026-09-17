#include <memory>
#include <string>
#include <utility>

#include "exchange.h"
#include "feed.h"
#include "location.h"
#include "pool.h"
#include "beast_client.h"

namespace
{
	// The connections of this thread and the io_context they run on. A node talks to the same
	// neighbours over and over, so the thread that forwarded to one before has the connection to
	// it still.
	http::pool &thread_pool()
	{
		thread_local http::pool held;

		return held;
	}

	std::string not_a_url(const std::string &url)
	{
		return "\"" + url + "\" is not a URL this client can send to.";
	}
}

http::beast_client::beast_client(long connect_timeout, long unacknowledged_timeout):
	connect_timeout_seconds(connect_timeout),
	unacknowledged_timeout_seconds(unacknowledged_timeout)
{
}

http::response http::beast_client::send(const request &request, long timeout_seconds) const
{
	response answer;
	location where = locate(request.url);

	if (!where.is_valid)
	{
		answer.message = not_a_url(request.url);

		return answer;
	}

	pool &held = thread_pool();
	std::unique_ptr<connection> link = held.take(where.host, where.port);
	exchange sending(
		*link,
		where,
		request,
		answer,
		timeout_seconds,
		connect_timeout_seconds,
		unacknowledged_timeout_seconds);

	sending.start();
	held.run();
	held.keep(std::move(link));

	return answer;
}

// Every request at once, on this thread's io_context, so a node writing a record to its copies
// waits for the slowest rather than for one after another — which is what keeps the thread it is
// serving on free.
std::vector<http::response> http::beast_client::send_all(const std::vector<request> &requests, long timeout_seconds)
	const
{
	std::vector<response> responses(requests.size());
	pool &held = thread_pool();
	std::vector<std::unique_ptr<connection>> links;
	std::vector<std::unique_ptr<exchange>> sending;

	for (size_t i = 0; i < requests.size(); i++)
	{
		location where = locate(requests[i].url);

		if (!where.is_valid)
		{
			responses[i].message = not_a_url(requests[i].url);

			continue;
		}

		links.push_back(held.take(where.host, where.port));

		sending.push_back(std::make_unique<exchange>(
			*links.back(),
			where,
			requests[i],
			responses[i],
			timeout_seconds,
			connect_timeout_seconds,
			unacknowledged_timeout_seconds));

		sending.back()->start();
	}

	held.run();

	for (auto &link : links)
	{
		held.keep(std::move(link));
	}

	return responses;
}

http::response http::beast_client::stream(
	const request &request,
	const std::function<bool(std::string_view)> &receive,
	const std::stop_token &stop) const
{
	response answer;
	location where = locate(request.url);

	if (!where.is_valid)
	{
		answer.message = not_a_url(request.url);

		return answer;
	}

	feed streaming(where, request, answer, receive, connect_timeout_seconds, unacknowledged_timeout_seconds);

	streaming.run(stop);

	return answer;
}
