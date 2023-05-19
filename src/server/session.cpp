#include <boost/beast/version.hpp>

#include <utility>
#include <string>

#include "error.h"
#include "log.h"
#include "session.h"

namespace
{
	boost::beast::http::response<boost::beast::http::string_body> make_response(
		unsigned version,
		bool keep_alive,
		const boost::beast::http::status &status,
		const std::string &body)
	{
		boost::beast::http::response<boost::beast::http::string_body> response {
			status,
			version,
			body
		};

		response.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
		response.set(boost::beast::http::field::content_type, "application/json");
		response.keep_alive(keep_alive);
		response.content_length(body.size());
		response.prepare_payload();

		return response;
	}

	boost::beast::http::response<boost::beast::http::string_body> bad_request(
		const boost::beast::http::request<boost::beast::http::string_body> &request,
		const std::string &why)
	{
		std::string body = boost::json::serialize(boost::json::object { { "error", why } });

		return make_response(request.version(), request.keep_alive(), boost::beast::http::status::bad_request, body);
	}

	boost::beast::http::response<boost::beast::http::string_body> server_error(
		const boost::beast::http::request<boost::beast::http::string_body> &request,
		const std::string &why)
	{
		std::string body = boost::json::serialize(boost::json::object { { "error", "Failed to respond due to error: " + why + "." } });

		return make_response(request.version(), request.keep_alive(), boost::beast::http::status::internal_server_error, body);
	}
}

server::session::session(boost::asio::ip::tcp::socket&& socket, router::router &router):
	stream(std::move(socket)),
	router(router)
{
	DEBUG("Session started.");
}

void server::session::run()
{
	read();
}

void server::session::on_read(boost::beast::error_code error, std::size_t bytes_transferred)
{
	boost::ignore_unused(bytes_transferred);

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

void server::session::on_write(bool keep_alive, boost::beast::error_code error, std::size_t bytes_transferred)
{
	boost::ignore_unused(bytes_transferred);
	DEBUG("keep alive: " + std::to_string(keep_alive));

	if (error)
	{
		throw std::runtime_error(ERROR("Error writing response: " + error.message()));
	}
	else if (keep_alive)
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

	boost::beast::http::async_read(
		stream,
		buffer,
		request,
		boost::beast::bind_front_handler(&session::on_read, shared_from_this()));
}

boost::beast::http::response<boost::beast::http::string_body> server::session::handle_request() const
{
	if (request.method() != boost::beast::http::verb::get &&
		request.method() != boost::beast::http::verb::post &&
		request.method() != boost::beast::http::verb::head)
	{
		return bad_request(request, "Unknown HTTP method.");
	}

	// Request path must be absolute and not contain "..".
	if (request.target().empty() ||
		request.target()[0] != '/' ||
		request.target().find("..") != boost::beast::string_view::npos)
	{
		return bad_request(request, "Invalid request path.");
	}

	router::response response;

	try
	{
		if (request.method() == boost::beast::http::verb::post)
		{
			response = router.post(std::string(request.target()), boost::json::parse(request.body()).as_object());
		}
		else
		{
			response = router.get(std::string(request.target()));
		}
	}
	catch (const std::exception &error)
	{
		return server_error(request, std::string(error.what()));
	}

	std::string body = boost::json::serialize(response.body);
	boost::beast::http::status status = response.status;

	return make_response(request.version(), request.keep_alive(), status, body);
}

void server::session::close()
{
	boost::beast::error_code ec;

	stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);

	DEBUG("Session ended.");
}
