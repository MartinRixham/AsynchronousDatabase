#include <chrono>

#include <boost/asio/post.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>

#include "log.h"
#include "bound_idle.h"
#include "headers.h"
#include "feed.h"

http::feed::feed(
	const location &where,
	const request &sending,
	response &answering,
	const std::function<bool(std::string_view)> &receiver,
	long connect_timeout,
	long unacknowledged_timeout):
	link(io, where.host, where.port),
	sent(sending),
	answer(answering),
	receive(receiver),
	connect_timeout_seconds(connect_timeout),
	unacknowledged_timeout_seconds(unacknowledged_timeout),
	carried(message_of(where, sending))
{
	parser.header_limit(header_bound);
	parser.body_limit(boost::none);
}

void http::feed::run(const std::stop_token &stop)
{
	// Stopping is asked for on another thread, so what it does is posted to the context rather
	// than done where it was asked: a socket closed under the read on it is a race. The callback
	// is registered for this call alone, so nothing it names outlives it.
	std::stop_callback stopping(
		stop,
		[this]()
		{
			boost::asio::post(
				io,
				[this]()
				{
					stopped = true;

					link.close();
				});
		});

	connect();

	io.run();
}

void http::feed::connect()
{
	boost::system::error_code error;
	boost::asio::ip::tcp::resolver::results_type addresses = link.addresses(error);

	if (error)
	{
		fail(error);

		return;
	}

	link.stream().expires_after(std::chrono::seconds(connect_timeout_seconds));
	link.stream().async_connect(
		addresses,
		[this](const boost::system::error_code &failed, const boost::asio::ip::tcp::endpoint &)
		{
			if (failed)
			{
				fail(failed);

				return;
			}

			// A stream sends nothing once it has asked, so what was sent and never acknowledged
			// says nothing about the node at the other end: a probe is something sent, and a node
			// gone from the network is one that acknowledges none of them.
			bound_idle(link.stream().socket().native_handle(), unacknowledged_timeout_seconds);

			link.opened();

			write();
		});
}

void http::feed::write()
{
	if (stopped)
	{
		return;
	}

	// An answer that does not end has nothing a timeout would mean, so the only bound on one is
	// the stopping the caller holds.
	link.stream().expires_never();

	boost::beast::http::async_write(
		link.stream(),
		carried,
		[this](const boost::system::error_code &failed, size_t)
		{
			if (failed)
			{
				fail(failed);

				return;
			}

			read_header();
		});
}

void http::feed::read_header()
{
	if (stopped)
	{
		return;
	}

	boost::beast::http::async_read_header(
		link.stream(),
		link.buffer(),
		parser,
		[this](const boost::system::error_code &failed, size_t)
		{
			if (failed)
			{
				fail(failed);

				return;
			}

			take_headers(parser.get(), answer);

			read_body();
		});
}

void http::feed::read_body()
{
	if (stopped || finished)
	{
		return;
	}

	parser.get().body().data = piece.data();
	parser.get().body().size = piece.size();

	boost::beast::http::async_read_some(
		link.stream(),
		link.buffer(),
		parser,
		[this](const boost::system::error_code &failed, size_t)
		{
			if (!hand_on(piece.size() - parser.get().body().size))
			{
				return;
			}

			// The body buffer being full is a read that got somewhere rather than one that
			// failed, and it is handed another.
			if (failed && failed != boost::beast::http::error::need_buffer)
			{
				fail(failed);

				return;
			}

			if (parser.is_done())
			{
				complete();

				return;
			}

			read_body();
		});
}

bool http::feed::hand_on(size_t taken)
{
	if (taken == 0)
	{
		return true;
	}

	if (receive(std::string_view(piece.data(), taken)))
	{
		return true;
	}

	end("The receiver ended the stream.");

	return false;
}

void http::feed::complete()
{
	finished = true;

	answer.is_valid = true;
	answer.message.clear();

	link.close();
}

void http::feed::end(const std::string &why)
{
	finished = true;

	answer.is_valid = false;
	answer.message = why;

	link.close();
}

void http::feed::fail(const boost::system::error_code &error)
{
	if (finished)
	{
		return;
	}

	end(stopped ? "The stream was stopped." : error.message());

	DEBUG("Stream of " + sent.url + " ended: " + answer.message);
}
