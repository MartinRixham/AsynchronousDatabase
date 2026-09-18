#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <strings.h>
#include <sys/socket.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/execution/context.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/query.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/span.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/span_body.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/lexical_cast/try_lexical_convert.hpp>

#include "log.h"
#include "connection.h"
#include "pool.h"
#include "shared_pool.h"
#include "beast_client.h"

namespace
{
	// Beast bounds the headers of an answer at eight kibibytes of its own, and an answer names the
	// key a walk is to resume at — base64 of a key that may be four kibibytes by itself.
	constexpr std::uint32_t header_bound = 64 * 1024;

	constexpr char scheme[] = "http://";

	constexpr size_t scheme_length = sizeof(scheme) - 1;

	// A request as Beast sends it. The body is the caller's and is not copied into it, which is
	// why the requests of a fan out have to outlive the call that carries them.
	using message = boost::beast::http::request<boost::beast::http::span_body<const char>>;

	// A URL as the pieces a request is sent with: a host to connect to and a target to ask that
	// host for, which is what Beast is given — it parses no URL of its own.
	struct location
	{
		std::string host;

		std::string port;

		// The path and the query as they arrived. What a segment holds was encoded where the
		// request was built, and is not this layer's to touch.
		std::string target;

		// What the request names itself as going to, the port included wherever there is one.
		std::string authority;
	};

	// Everything here is spoken over http, so anything else is no location rather than one to
	// guess at.
	std::optional<location> locate(const std::string &url)
	{
		if (url.compare(0, scheme_length, scheme) != 0)
		{
			return std::nullopt;
		}

		location where;
		size_t end = url.find_first_of("/?", scheme_length);

		where.authority = url.substr(scheme_length, end == std::string::npos ? std::string::npos : end - scheme_length);

		if (end == std::string::npos)
		{
			where.target = "/";
		}
		else
		{
			where.target = url[end] == '/' ? url.substr(end) : "/" + url.substr(end);
		}

		// An address in brackets is IPv6 and holds colons of its own, so what names the port is
		// the colon after the brackets rather than the last one.
		size_t bracket = where.authority.rfind(']');
		size_t colon = where.authority.rfind(':');

		if (colon != std::string::npos && (bracket == std::string::npos || colon > bracket))
		{
			where.host = where.authority.substr(0, colon);
			where.port = where.authority.substr(colon + 1);
		}
		else
		{
			where.host = where.authority;
			where.port = "80";
		}

		if (bracket != std::string::npos && where.host.size() > 1 && where.host.front() == '[')
		{
			where.host = where.host.substr(1, bracket - 1);
		}

		if (where.host.empty() || where.port.empty())
		{
			return std::nullopt;
		}

		return where;
	}

	std::string not_a_url(const std::string &url)
	{
		return "\"" + url + "\" is not a URL this client can send to.";
	}

	// A line the caller wrote, as the name and the value Beast holds them as. A line with nothing
	// to separate the two is no header at all.
	void set_header(boost::beast::http::fields &fields, const std::string &line)
	{
		size_t colon = line.find(':');

		if (colon == std::string::npos || colon == 0)
		{
			return;
		}

		size_t start = line.find_first_not_of(" \t", colon + 1);

		fields.set(
			line.substr(0, colon),
			start == std::string::npos ? "" : line.substr(start, line.find_last_not_of(" \t") + 1 - start));
	}

	// Of a request whose URL has already been found to be one.
	message message_of(const http::request &sending)
	{
		location where = locate(sending.url).value();
		message carried;
		boost::beast::http::verb method = boost::beast::http::string_to_verb(sending.method);

		if (method == boost::beast::http::verb::unknown)
		{
			carried.method_string(sending.method);
		}
		else
		{
			carried.method(method);
		}

		carried.target(where.target);
		carried.version(11);
		carried.set(boost::beast::http::field::host, where.authority);
		carried.body() = boost::beast::span<const char>(sending.body.data(), sending.body.size());

		for (const auto &line : sending.headers)
		{
			set_header(carried, line);
		}

		// A body large enough to be worth a handshake would otherwise wait for a 100 Continue that
		// only the proxy in front of a node ever answers, so nothing here asks for one.
		carried.prepare_payload();

		return carried;
	}

	bool is_ours(const boost::beast::http::fields::value_type &field)
	{
		std::string_view name(field.name_string().data(), field.name_string().size());

		return name.size() >= std::strlen(http::header_prefix) &&
			   strncasecmp(name.data(), http::header_prefix, std::strlen(http::header_prefix)) == 0;
	}

	std::pair<std::string, std::string> named(const boost::beast::http::fields::value_type &field)
	{
		return std::pair<std::string, std::string>(
			std::string(field.name_string().data(), field.name_string().size()),
			std::string(field.value().data(), field.value().size()));
	}

	// What an answer says before its body. The body is not read here: one caller keeps it whole
	// and the other hands it on as it arrives.
	void take_headers(const boost::beast::http::response_header<> &received, http::response &answer)
	{
		answer.status = received.result_int();

		boost::beast::string_view type = received[boost::beast::http::field::content_type];
		boost::beast::string_view length = received[boost::beast::http::field::content_length];

		answer.content_type = std::string(type.data(), type.size());

		// An answer that carried no length at all says nothing about how large the body is, which
		// is a length of none rather than one to guess at.
		answer.content_length = 0;

		boost::conversion::try_lexical_convert(std::string(length.data(), length.size()), answer.content_length);

		std::ranges::copy(
			received | std::views::filter(is_ours) | std::views::transform(named),
			std::back_inserter(answer.headers));
	}

	// TCP keepalive: a connection nothing is sent on is probed this often, and one that answers no
	// probe is ended.
	void bound_idle(int socket, long seconds)
	{
		int on = 1;
		int interval = static_cast<int>(seconds);

		setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
		setsockopt(socket, IPPROTO_TCP, TCP_KEEPIDLE, &interval, sizeof(interval));
		setsockopt(socket, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
	}

	// TCP_USER_TIMEOUT: what was sent on a connection and never acknowledged ends it after this long.
	void bound_unacknowledged(int socket, long seconds)
	{
		unsigned int milliseconds = static_cast<unsigned int>(seconds * 1000);

		setsockopt(socket, IPPROTO_TCP, TCP_USER_TIMEOUT, &milliseconds, sizeof(milliseconds));
	}

	// A node talks to the same neighbours over and over, so the thread that forwarded to one
	// before has the connection to it still.
	http::pool &thread_pool()
	{
		thread_local http::pool held;

		return held;
	}

	// One request and the answer to it, on the io_context the connection belongs to. A timeout of
	// none bounds nothing but connecting.
	boost::asio::awaitable<std::expected<http::response, std::string>> exchange(
		http::connection &link,
		const http::request &sent,
		long timeout_seconds,
		long connect_timeout_seconds,
		long unacknowledged_timeout_seconds)
	{
		http::response answer;
		message carried = message_of(sent);

		// The whole transfer, connecting included.
		std::optional<std::chrono::steady_clock::time_point> deadline;

		if (timeout_seconds > 0)
		{
			deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
		}

		answer.reused = link.is_open();

		for (;;)
		{
			boost::beast::http::response_parser<boost::beast::http::string_body> parser;
			boost::system::error_code failed;

			// A HEAD is answered with the headers of the GET and none of its body, so the length
			// the answer names is what the body would have been rather than what is coming.
			parser.skip(carried.method() == boost::beast::http::verb::head);
			parser.header_limit(header_bound);

			// A file of records is as large as the node sending it was asked for, so what bounds
			// an answer is the budget the request carried and not a limit of the client's own.
			parser.body_limit(boost::none);

			try
			{
				if (!link.is_open())
				{
					std::chrono::steady_clock::time_point bound =
						std::chrono::steady_clock::now() + std::chrono::seconds(connect_timeout_seconds);

					co_await link.connect(deadline ? std::min(*deadline, bound) : bound);

					// A node that went from the network after the connection was made acknowledges
					// nothing it is sent, where a node that is slow still does.
					bound_unacknowledged(link.stream().socket().native_handle(), unacknowledged_timeout_seconds);
				}

				if (deadline)
				{
					link.stream().expires_at(*deadline);
				}
				else
				{
					link.stream().expires_never();
				}

				co_await boost::beast::http::async_write(link.stream(), carried, boost::asio::use_awaitable);
				co_await boost::beast::http::async_read(
					link.stream(),
					link.buffer(),
					parser,
					boost::asio::use_awaitable);
			}
			catch (const boost::system::system_error &error)
			{
				failed = error.code();
			}

			if (!failed)
			{
				take_headers(parser.get(), answer);

				answer.body = std::move(parser.get().body());

				// A timer left armed on a connection nobody is using is the next request on it
				// timing out before it was sent.
				link.stream().expires_never();

				if (!parser.get().keep_alive())
				{
					link.close();
				}

				co_return answer;
			}

			link.close();

			// A connection out of the pool may have been closed by the node at the other end since
			// the pool last looked at it, which is a request to make again rather than a request
			// that failed. Only once, and only while nothing of the answer has been read.
			if (!answer.reused || parser.got_some())
			{
				DEBUG("Request to " + sent.url + " failed: " + failed.message());

				co_return std::unexpected(failed.message());
			}

			answer.reused = false;
		}
	}

	// An answer that does not end on its own. Each piece of the body is handed to the receiver as
	// it arrives rather than kept, so there is no timeout but connecting. What stops a feed is its
	// connection closed under it, and the token only says that was asked for rather than failed.
	boost::asio::awaitable<std::expected<http::response, std::string>> feed(
		http::connection &link,
		const http::request &sent,
		const std::function<bool(std::string_view)> &receive,
		std::stop_token stop,
		long connect_timeout_seconds,
		long idle_timeout_seconds)
	{
		http::response answer;
		message carried = message_of(sent);
		boost::beast::http::response_parser<boost::beast::http::buffer_body> parser;

		// What a piece of the body is read into before it is handed on. A piece is whatever
		// arrived rather than a whole document, and the receiver is what puts them back together.
		// It is not an array in the coroutine's frame because GCC reports a frame that large as a
		// mismatched delete.
		std::vector<char> piece(16 * 1024);

		parser.header_limit(header_bound);
		parser.body_limit(boost::none);

		try
		{
			co_await link.connect(std::chrono::steady_clock::now() + std::chrono::seconds(connect_timeout_seconds));

			// A stream sends nothing once it has asked, so what was sent and never acknowledged
			// says nothing about the node at the other end: a probe is something sent, and a node
			// gone from the network is one that acknowledges none of them.
			bound_idle(link.stream().socket().native_handle(), idle_timeout_seconds);

			// An answer that does not end has nothing a timeout would mean.
			link.stream().expires_never();

			co_await boost::beast::http::async_write(link.stream(), carried, boost::asio::use_awaitable);
			co_await boost::beast::http::async_read_header(
				link.stream(),
				link.buffer(),
				parser,
				boost::asio::use_awaitable);

			take_headers(parser.get(), answer);

			while (!parser.is_done())
			{
				parser.get().body().data = piece.data();
				parser.get().body().size = piece.size();

				auto [failed, ignored] = co_await boost::beast::http::async_read_some(
					link.stream(),
					link.buffer(),
					parser,
					boost::asio::as_tuple(boost::asio::use_awaitable));
				size_t taken = piece.size() - parser.get().body().size;

				if (taken > 0 && !receive(std::string_view(piece.data(), taken)))
				{
					co_return std::unexpected("The receiver ended the stream.");
				}

				// The body buffer being full is a read that got somewhere rather than one that
				// failed, and it is handed another.
				if (failed && failed != boost::beast::http::error::need_buffer)
				{
					throw boost::system::system_error(failed);
				}
			}
		}
		catch (const boost::system::system_error &error)
		{
			std::string ended = stop.stop_requested() ? "The stream was stopped." : error.code().message();

			DEBUG("Stream of " + sent.url + " ended: " + ended);

			co_return std::unexpected(ended);
		}

		co_return answer;
	}

	// What a transfer answered, put where the caller keeps it. A transfer answers every failure of
	// the network's as its error, so what it throws is the caller's and is thrown on out of run().
	auto answer_into(std::expected<http::response, std::string> &kept)
	{
		return [&kept](const std::exception_ptr &thrown, std::expected<http::response, std::string> answered)
		{
			if (thrown)
			{
				std::rethrow_exception(thrown);
			}

			kept = std::move(answered);
		};
	}

	// Every request at once, on this thread's io_context, so a node writing a record to its copies
	// waits for the slowest rather than for one after another — which is what keeps the thread it
	// is serving on free.
	std::vector<std::expected<http::response, std::string>> fan_out(
		std::span<const http::request> requests,
		long timeout_seconds,
		long connect_timeout_seconds,
		long unacknowledged_timeout_seconds)
	{
		std::vector<std::expected<http::response, std::string>> responses(requests.size());
		http::pool &held = thread_pool();
		std::vector<std::unique_ptr<http::connection>> links;

		for (size_t i = 0; i < requests.size(); i++)
		{
			std::optional<location> where = locate(requests[i].url);

			if (!where)
			{
				responses[i] = std::unexpected(not_a_url(requests[i].url));

				continue;
			}

			links.push_back(held.take(where->host, where->port));

			boost::asio::co_spawn(
				held.context(),
				exchange(
					*links.back(),
					requests[i],
					timeout_seconds,
					connect_timeout_seconds,
					unacknowledged_timeout_seconds),
				answer_into(responses[i]));
		}

		held.run();

		for (auto &link : links)
		{
			held.keep(std::move(link));
		}

		return responses;
	}
}

http::beast_client::beast_client(long connect_timeout, long unacknowledged_timeout):
	connect_timeout_seconds(connect_timeout),
	unacknowledged_timeout_seconds(unacknowledged_timeout)
{
}

std::expected<http::response, std::string> http::beast_client::send(const request &request, long timeout_seconds) const
{
	return fan_out(std::span(&request, 1), timeout_seconds, connect_timeout_seconds, unacknowledged_timeout_seconds)
		.front();
}

std::vector<std::expected<http::response, std::string>> http::beast_client::send_all(
	const std::vector<request> &requests,
	long timeout_seconds) const
{
	return fan_out(requests, timeout_seconds, connect_timeout_seconds, unacknowledged_timeout_seconds);
}

// On the connections the io_context of the coroutine awaiting it holds rather than the thread's,
// because that coroutine may resume on any thread of it.
boost::asio::awaitable<std::expected<http::response, std::string>> http::beast_client::async_send(
	const request &request,
	long timeout_seconds) const
{
	std::optional<location> where = locate(request.url);

	if (!where)
	{
		co_return std::unexpected(not_a_url(request.url));
	}

	boost::asio::any_io_executor executor = co_await boost::asio::this_coro::executor;
	shared_pool &held =
		boost::asio::use_service<shared_pool>(boost::asio::query(executor, boost::asio::execution::context));
	std::unique_ptr<connection> link = held.take(executor, where->host, where->port);

	std::expected<response, std::string> answer =
		co_await exchange(*link, request, timeout_seconds, connect_timeout_seconds, unacknowledged_timeout_seconds);

	held.keep(std::move(link));

	co_return answer;
}

// On a connection of its own: an answer still coming down one is nothing to hand back to a pool.
std::expected<http::response, std::string> http::beast_client::stream(
	const request &request,
	const std::function<bool(std::string_view)> &receive,
	const std::stop_token &stop) const
{
	std::expected<response, std::string> answer;
	std::optional<location> where = locate(request.url);

	if (!where)
	{
		return std::unexpected(not_a_url(request.url));
	}

	boost::asio::io_context io;
	http::connection link(io.get_executor(), where->host, where->port);

	boost::asio::co_spawn(
		io,
		feed(link, request, receive, stop, connect_timeout_seconds, unacknowledged_timeout_seconds),
		answer_into(answer));

	// Stopping is asked for on another thread, so what it does is posted to the context rather than
	// done where it was asked: a socket closed under the read on it is a race. It is registered
	// after the feed is spawned, so the close is queued behind the feed's start rather than ahead
	// of a connection the feed would then open.
	std::stop_callback stopping(stop, [&io, &link]() { boost::asio::post(io, [&link]() { link.close(); }); });

	io.run();

	return answer;
}
