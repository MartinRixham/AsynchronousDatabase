#include "session.h"
#include "listener.h"

listener::listener(boost::asio::io_context& ioc, boost::asio::ip::tcp::endpoint endpoint):
    io_context(ioc),
    acceptor(boost::asio::make_strand(ioc))
{
    boost::beast::error_code error;

    acceptor.open(endpoint.protocol(), error);

    if (error)
    {
        fail(error, "open");
        return;
    }

    acceptor.set_option(boost::asio::socket_base::reuse_address(true), error);

    if (error)
    {
        fail(error, "set_option");
        return;
    }

    acceptor.bind(endpoint, error);

    if (error)
    {
        fail(error, "bind");
        return;
    }

    acceptor.listen(boost::asio::socket_base::max_listen_connections, error);

    if (error)
    {
        fail(error, "listen");
        return;
    }

    port_number = acceptor.local_endpoint().port();
}

void listener::run()
{
    accept();
}

boost::asio::ip::port_type listener::port()
{
    return port_number;
}

void listener::accept()
{
    // The new connection gets its own strand
    acceptor.async_accept(
        boost::asio::make_strand(io_context),
        boost::beast::bind_front_handler(&listener::on_accept, shared_from_this()));
}

void listener::on_accept(boost::beast::error_code error, boost::asio::ip::tcp::socket socket)
{
    if (error)
    {
        fail(error, "accept");
    }
    else
    {
        std::make_shared<session>(std::move(socket))->run();
    }

    accept();
}
