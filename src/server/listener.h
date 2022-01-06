#ifndef SERVER_LISTENER_H
#define SERVER_LISTENER_H

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>

namespace server
{
    class listener : public std::enable_shared_from_this<listener>
    {
        boost::asio::io_context &io_context;

        boost::asio::ip::tcp::acceptor acceptor;

        boost::asio::ip::port_type port_number;

    public:
        listener(boost::asio::io_context& io_context, boost::asio::ip::tcp::endpoint endpoint);

        void run();

        boost::asio::ip::port_type port();

    private:
        void accept();

        void on_accept(boost::beast::error_code error, boost::asio::ip::tcp::socket socket);
    };
}

#endif
