#include <algorithm>
#include <utility>
#include <string>

#include <boost/beast/version.hpp>

#include "log.h"
#include "url/url.h"
#include "session.h"

namespace
{
	boost::beast::http::response<boost::beast::http::string_body> make_response(
		unsigned version,
		bool keep_alive,
		const boost::beast::http::status &status,
		const std::string &content_type,
		const std::string &body,
		bool head)
	{
		boost::beast::http::response<boost::beast::http::string_body> response {
			status,
			version,
			head ? "" : body
		};

		response.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);

		if (!content_type.empty())
		{
			response.set(boost::beast::http::field::content_type, content_type);
		}

		response.keep_alive(keep_alive);
		response.prepare_payload();

		// HEAD answers the same headers with no body, and the length is the cheap way to ask how
		// large a value is.
		if (head && !body.empty())
		{
			response.set(boost::beast::http::field::content_length, std::to_string(body.size()));
		}

		return response;
	}

	bool is_routable_method(const boost::beast::http::verb &method)
	{
		return method == boost::beast::http::verb::get ||
			method == boost::beast::http::verb::head ||
			method == boost::beast::http::verb::put ||
			method == boost::beast::http::verb::delete_;
	}

	// Only a segment that is entirely ".." is a traversal. A key that happens to contain dots is
	// a key like any other, and travels percent encoded.
	bool has_traversal(const std::string &target)
	{
		std::vector<std::string> path = url::split_path(target.substr(0, target.find('?')));

		return std::any_of(path.begin(), path.end(), [](const std::string &segment) { return segment == ".."; });
	}
}

server::session::session(
	boost::asio::ip::tcp::socket&& socket,
	router::router &router,
	const std::atomic<bool> &stopping):
		stream(std::move(socket)),
		router(router),
		stopping(stopping)
{
	DEBUG("Session started.");
}

void server::session::run()
{
	read();
}

void server::session::stop()
{
	// The stream belongs to the connection's own executor, so it is asked there rather than on
	// whichever thread is shutting the server down.
	boost::asio::dispatch(
		stream.get_executor(),
		[self = shared_from_this()]()
		{
			if (self->waiting)
			{
				// The pending read ends in an error, which is the path that closes a session.
				self->stream.cancel();
			}
		});
}

void server::session::on_read(boost::beast::error_code error, std::size_t)
{
	waiting = false;

	if (error)
	{
		DEBUG("Closing connection: " + error.message());
		close();

		return;
	}

	http_response = std::make_shared<boost::beast::http::response<boost::beast::http::string_body>>(handle_request());

	boost::beast::http::async_write(
		stream,
		*http_response,
		boost::beast::bind_front_handler(
			&session::on_write,
			shared_from_this(),
			http_response->keep_alive()));
}

void server::session::on_write(bool keep_alive, boost::beast::error_code error, std::size_t)
{
	DEBUG("keep alive: " + std::to_string(keep_alive));

	if (error)
	{
		DEBUG("Error writing response: " + error.message());

		close();
	}
	else if (keep_alive && !stopping)
	{
		http_response = nullptr;

		read();
	}
	else
	{
		close();
	}
}

void server::session::read()
{
	request = {};

	stream.expires_after(std::chrono::seconds(60));

	waiting = true;

	boost::beast::http::async_read(
		stream,
		buffer,
		request,
		boost::beast::bind_front_handler(&session::on_read, shared_from_this()));
}

boost::beast::http::response<boost::beast::http::string_body> server::session::handle_request() const
{
	bool head = request.method() == boost::beast::http::verb::head;
	std::string target(request.target());
	router::response response;

	if (!is_routable_method(request.method()))
	{
		response = router::error_response(
			"method_not_allowed", std::string(request.method_string()) + " is not a method of this API.");
	}
	else if (target.empty() || target[0] != '/' || has_traversal(target))
	{
		response = router::error_response("invalid_path", "Invalid request path.");
	}
	else
	{
		router::request routed {
			request.method(),
			url::split_path(target),
			url::query_string(target),
			request.body(),
			!request[cluster::forwarded_header].empty()
		};

		try
		{
			response = router.route(routed);
		}
		catch (const repository::storage_error &error)
		{
			response = router::error_response(error.code(), error.what());
		}
		catch (const std::exception &error)
		{
			response = router::error_response(
				"storage_error", "Failed to respond due to error: " + std::string(error.what()) + ".");
		}
	}

	// A connection is not kept alive into a shutdown: the client is told to close, and finds
	// another node or comes back to this one.
	return make_response(
		request.version(),
		request.keep_alive() && !stopping,
		response.status,
		response.content_type,
		router::response_body(response),
		head);
}

void server::session::close()
{
	boost::beast::error_code ec;

	stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);

	DEBUG("Session ended.");
}
