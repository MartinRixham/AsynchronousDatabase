#ifndef SERVER_SESSION_H
#define SERVER_SESSION_H

#include <memory>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "router/router.h"

void fail(boost::beast::error_code ec, char const* what);

namespace server
{
	class session : public std::enable_shared_from_this<session>
	{
		boost::beast::tcp_stream stream;

		boost::beast::flat_buffer buffer;

		boost::beast::http::request<boost::beast::http::string_body> request;

		std::shared_ptr<boost::beast::http::response<boost::beast::http::string_body>> http_response;

		router::router &router;

	public:
		explicit session(boost::asio::ip::tcp::socket&& socket, router::router &router);

		void run();

		void on_read(boost::beast::error_code error, std::size_t);

		void on_write(bool should_close, boost::beast::error_code error, std::size_t);

	private:
		void read();

		boost::beast::http::response<boost::beast::http::string_body> handle_request() const;

		void close();
	};
}

#endif
