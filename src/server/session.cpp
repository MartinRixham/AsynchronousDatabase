#include <boost/beast/version.hpp>

#include <memory>

#include "error.h"
#include "session.h"

server::session::session(boost::asio::ip::tcp::socket&& socket, router::router &router):
	stream(std::move(socket)),
	router(router)
{
}

void server::session::run()
{
	read();
}

void server::session::on_read(boost::beast::error_code error, std::size_t bytes_transferred)
{
	boost::ignore_unused(bytes_transferred);

	if (error == boost::beast::http::error::end_of_stream)
	{
		return close();
	}
	else if (error)
	{
		throw std::runtime_error(ERROR(error.message()));
	}

	http_response = std::make_shared<boost::beast::http::response<boost::beast::http::string_body>>(handle_request());

	boost::beast::http::async_write(
		stream,
		*http_response,
		boost::beast::bind_front_handler(
			&session::on_write,
			shared_from_this(),
			http_response->need_eof()));
}

void server::session::on_write(bool should_close, boost::beast::error_code error, std::size_t bytes_transferred)
{
	boost::ignore_unused(bytes_transferred);

	if (error)
	{
		throw std::runtime_error(ERROR(error.message()));
	}
	else if (should_close)
	{
		return close();
	}
	else
	{
		http_response = nullptr;

		read();
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

boost::beast::http::response<boost::beast::http::string_body> server::session::bad_request(boost::beast::string_view why)
{
	boost::beast::http::response<boost::beast::http::string_body> bad_request_response{
		boost::beast::http::status::bad_request, request.version()};

	bad_request_response.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
	bad_request_response.set(boost::beast::http::field::content_type, "text/html");
	bad_request_response.keep_alive(request.keep_alive());
	bad_request_response.body() = std::string(why);
	bad_request_response.prepare_payload();

	return bad_request_response;
}

boost::beast::http::response<boost::beast::http::string_body> server::session::handle_request()
{
	if (request.method() != boost::beast::http::verb::get &&
		request.method() != boost::beast::http::verb::post &&
		request.method() != boost::beast::http::verb::head)
	{
		return bad_request("Unknown HTTP-method");
	}

	// Request path must be absolute and not contain "..".
	if (request.target().empty() ||
		request.target()[0] != '/' ||
		request.target().find("..") != boost::beast::string_view::npos)
	{
		return bad_request("Illegal request-target");
	}

	router::response response;

	if (request.method() == boost::beast::http::verb::post)
	{
		response = router.post(std::string(request.target()), boost::json::parse(request.body()).as_object());
	}
	else
	{
		response = router.get(std::string(request.target()));
	}

	std::string body = boost::json::serialize(response.body);
	boost::beast::http::status status = response.status;

	if (request.method() == boost::beast::http::verb::head)
	{
		boost::beast::http::response<boost::beast::http::string_body> ok_response{
			status,
			request.version()};

		ok_response.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
		ok_response.set(boost::beast::http::field::content_type, "application/json");
		ok_response.content_length(body.size());
		ok_response.keep_alive(request.keep_alive());

		return ok_response;
	}
	else
	{
		boost::beast::http::response<boost::beast::http::string_body> ok_response{
			std::piecewise_construct,
			std::make_tuple(body),
			std::make_tuple(status, request.version())};

		ok_response.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
		ok_response.set(boost::beast::http::field::content_type, "application/json");
		ok_response.content_length(body.size());
		ok_response.keep_alive(request.keep_alive());

		return ok_response;
	}
}

void server::session::close()
{
	boost::beast::error_code ec;
	stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
}
