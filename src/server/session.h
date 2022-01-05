#ifndef SERVER_SESSION_H
#define SERVER_SESSION_H

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

void fail(boost::beast::error_code ec, char const* what);

class session : public std::enable_shared_from_this<session>
{
    boost::beast::tcp_stream stream;

    boost::beast::flat_buffer buffer;

    boost::beast::http::request<boost::beast::http::string_body> request;

    std::shared_ptr<boost::beast::http::response<boost::beast::http::string_body>> response;

public:
    explicit session(boost::asio::ip::tcp::socket&& socket);

    void run();

    void read();

    boost::beast::http::response<boost::beast::http::string_body> bad_request(boost::beast::string_view why);

    boost::beast::http::response<boost::beast::http::string_body> handle_request();

    void on_read(boost::beast::error_code error, std::size_t bytes_transferred);

    void on_write(bool should_close, boost::beast::error_code error, std::size_t bytes_transferred);

    void close();
};

#endif
