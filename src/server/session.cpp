#include <boost/beast/version.hpp>

#include <memory>
#include <iostream>

#include "session.h"

void fail(boost::beast::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

session::session(boost::asio::ip::tcp::socket&& socket):
    stream(std::move(socket))
{
}

void session::run()
{
    read();
}

void session::read()
{
    request = {};

    stream.expires_after(std::chrono::seconds(60));

    boost::beast::http::async_read(
        stream,
        buffer,
        request,
        boost::beast::bind_front_handler(&session::on_read, shared_from_this()));
}

boost::beast::http::response<boost::beast::http::string_body> session::bad_request(boost::beast::string_view why)
{
    boost::beast::http::response<boost::beast::http::string_body> response{
        boost::beast::http::status::bad_request, request.version()};

    response.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
    response.set(boost::beast::http::field::content_type, "text/html");
    response.keep_alive(request.keep_alive());
    response.body() = std::string(why);
    response.prepare_payload();

    return response;
}

boost::beast::http::response<boost::beast::http::string_body> session::handle_request()
{
    if (request.method() != boost::beast::http::verb::get &&
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

    std::string body("{ \"message\": \"Hello, world!\" }");

    if (request.method() == boost::beast::http::verb::head)
    {
        boost::beast::http::response<boost::beast::http::string_body> response{
            boost::beast::http::status::ok, request.version()};

        response.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
        response.set(boost::beast::http::field::content_type, "application/json");
        response.content_length(body.size());
        response.keep_alive(request.keep_alive());

        return response;
    }
    else
    {
        boost::beast::http::response<boost::beast::http::string_body> response{
            std::piecewise_construct,
            std::make_tuple(body),
            std::make_tuple(boost::beast::http::status::ok, request.version())};

        response.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
        response.set(boost::beast::http::field::content_type, "application/json");
        response.content_length(body.size());
        response.keep_alive(request.keep_alive());

        return response;
    }
}

void session::on_read(boost::beast::error_code error, std::size_t bytes_transferred)
{
    boost::ignore_unused(bytes_transferred);

    if (error == boost::beast::http::error::end_of_stream)
    {
        return close();
    }

    if (error)
    {
        return fail(error, "read");
    }

    response = std::make_shared<boost::beast::http::response<boost::beast::http::string_body>>(handle_request());

    boost::beast::http::async_write(
        stream,
        *response,
        boost::beast::bind_front_handler(
            &session::on_write,
            shared_from_this(),
            response->need_eof()));
}

void session::on_write(bool should_close, boost::beast::error_code error, std::size_t bytes_transferred)
{
    boost::ignore_unused(bytes_transferred);

    if (error)
    {
        return fail(error, "write");
    }
    else if (should_close)
    {
        return close();
    }
    else
    {
        response = nullptr;

        read();
    }
}

void session::close()
{
    boost::beast::error_code ec;
    stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
}
