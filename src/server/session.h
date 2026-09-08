#ifndef SERVER_SESSION_H
#define SERVER_SESSION_H

#include <atomic>
#include <memory>
#include <optional>

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

		std::optional<boost::beast::http::request_parser<boost::beast::http::string_body>> parser;

		std::shared_ptr<boost::beast::http::response<boost::beast::http::string_body>> http_response;

		router::router &router;

		// The server is shutting down. A connection is not kept alive past the response it is
		// writing, and one that is waiting for a request that may never come is cut.
		const std::atomic<bool> &stopping;

		// True between asking for a request and receiving one, which is the only state a session
		// can be left in for as long as a client cares to leave it there.
		bool waiting = false;

	public:
		session(
			boost::asio::ip::tcp::socket&& socket,
			router::router &router,
			const std::atomic<bool> &stopping);

		void run();

		void stop();

		void on_read(boost::beast::error_code error, std::size_t);

		void on_write(bool should_close, boost::beast::error_code error, std::size_t);

	private:
		void read();

		boost::beast::http::response<boost::beast::http::string_body> handle_request() const;

		void close();
	};
}

#endif
