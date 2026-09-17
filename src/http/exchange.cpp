#include <algorithm>
#include <utility>

#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>

#include "log.h"
#include "bound_unacknowledged.h"
#include "headers.h"
#include "exchange.h"

http::exchange::exchange(
	connection &held,
	const location &where,
	const request &sending,
	response &answering,
	long timeout_seconds,
	long connect_timeout,
	long unacknowledged_timeout):
	link(held),
	sent(sending),
	answer(answering),
	connect_timeout_seconds(connect_timeout),
	unacknowledged_timeout_seconds(unacknowledged_timeout),
	bounded(timeout_seconds > 0),
	deadline(std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds)),
	carried(message_of(where, sending))
{
}

void http::exchange::start()
{
	answer.reused = link.is_open();

	if (link.is_open())
	{
		write();
	}
	else
	{
		connect();
	}
}

void http::exchange::connect()
{
	boost::system::error_code error;
	boost::asio::ip::tcp::resolver::results_type addresses = link.addresses(error);

	if (error)
	{
		fail(error);

		return;
	}

	// Connecting is bounded apart from the transfer, and by whichever of the two comes first.
	std::chrono::steady_clock::time_point bound =
		std::chrono::steady_clock::now() + std::chrono::seconds(connect_timeout_seconds);

	link.stream().expires_at(bounded ? std::min(deadline, bound) : bound);
	link.stream().async_connect(
		addresses,
		[this](const boost::system::error_code &failed, const boost::asio::ip::tcp::endpoint &)
		{
			if (failed)
			{
				fail(failed);

				return;
			}

			// A node that went from the network after the connection was made acknowledges
			// nothing it is sent, where a node that is slow still does.
			bound_unacknowledged(link.stream().socket().native_handle(), unacknowledged_timeout_seconds);

			link.opened();

			write();
		});
}

void http::exchange::write()
{
	if (bounded)
	{
		link.stream().expires_at(deadline);
	}
	else
	{
		link.stream().expires_never();
	}

	boost::beast::http::async_write(
		link.stream(),
		carried,
		[this](const boost::system::error_code &failed, size_t)
		{
			if (failed)
			{
				if (!redial())
				{
					fail(failed);
				}

				return;
			}

			read();
		});
}

void http::exchange::read()
{
	parser.emplace();

	// A HEAD is answered with the headers of the GET and none of its body, so the length the
	// answer names is what the body would have been rather than what is coming.
	parser->skip(carried.method() == boost::beast::http::verb::head);
	parser->header_limit(header_bound);

	// A file of records is as large as the node sending it was asked for, so what bounds an
	// answer is the budget the request carried and not a limit of the client's own.
	parser->body_limit(boost::none);

	boost::beast::http::async_read(
		link.stream(),
		link.buffer(),
		*parser,
		[this](const boost::system::error_code &failed, size_t)
		{
			if (failed)
			{
				if (parser->got_some() || !redial())
				{
					fail(failed);
				}

				return;
			}

			complete();
		});
}

void http::exchange::complete()
{
	boost::beast::http::response<boost::beast::http::string_body> &received = parser->get();

	take_headers(received, answer);

	answer.body = std::move(received.body());
	answer.is_valid = true;
	answer.message.clear();

	// A timer left armed on a connection nobody is using is the next request on it timing out
	// before it was sent.
	link.stream().expires_never();

	if (!received.keep_alive())
	{
		link.close();
	}
}

bool http::exchange::redial()
{
	if (redialed || !answer.reused)
	{
		return false;
	}

	redialed = true;
	answer.reused = false;

	link.close();
	connect();

	return true;
}

void http::exchange::fail(const boost::system::error_code &error)
{
	answer.is_valid = false;
	answer.message = error.message();

	link.close();

	DEBUG("Request to " + sent.url + " failed: " + answer.message);
}
