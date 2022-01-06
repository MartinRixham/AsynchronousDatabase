#include <exception>

#include "session.h"
#include "listener.h"

#define ERROR(error) "" + std::string(__FILE__) + " " + std::string(__func__) + "(line:" + std::to_string(__LINE__) + ") " + (error)

server::listener::listener(boost::asio::io_context &io_context, boost::asio::ip::tcp::endpoint endpoint):
    io_context(io_context),
    acceptor(boost::asio::make_strand(io_context))
{
    boost::beast::error_code error;

    acceptor.open(endpoint.protocol(), error);
    acceptor.set_option(boost::asio::socket_base::reuse_address(true), error);
    acceptor.bind(endpoint, error);

    if (error)
    {
        throw std::runtime_error(ERROR(error.message()));
    }

    acceptor.listen(boost::asio::socket_base::max_listen_connections, error);

    if (error)
    {
        throw std::runtime_error(ERROR(error.message()));
    }

    port_number = acceptor.local_endpoint().port();
}

void server::listener::run()
{
    accept();
}

boost::asio::ip::port_type server::listener::port()
{
    return port_number;
}

void server::listener::accept()
{
    // The new connection gets its own strand
    acceptor.async_accept(
        boost::asio::make_strand(io_context),
        boost::beast::bind_front_handler(&listener::on_accept, shared_from_this()));
}

void server::listener::on_accept(boost::beast::error_code error, boost::asio::ip::tcp::socket socket)
{
    if (error)
    {
        throw std::runtime_error(ERROR(error.message()));
    }
    else
    {
        std::make_shared<session>(std::move(socket))->run();
    }

    accept();
}
