#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

#include <boost/asio/io_context.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/parser.hpp>

#include "connection.h"
#include "http_client.h"
#include "http_message.h"
#include "location.h"

namespace http
{
	// An answer that does not end on its own. Each piece of the body is handed to the receiver as
	// it arrives rather than kept, so there is no timeout but connecting, and the connection is
	// this call's own: an answer still coming down one is nothing to hand back to a pool.
	class feed
	{
		boost::asio::io_context io;

		connection link;

		const request &sent;

		response &answer;

		const std::function<bool(std::string_view)> &receive;

		long connect_timeout_seconds;

		long unacknowledged_timeout_seconds;

		message carried;

		boost::beast::http::response_parser<boost::beast::http::buffer_body> parser;

		// What a piece of the body is read into before it is handed on. A piece is whatever
		// arrived rather than a whole document, and the receiver is what puts them back together.
		std::array<char, 16 * 1024> piece = {};

		bool stopped = false;

		bool finished = false;

	public:
		feed(
			const location &where,
			const request &sending,
			response &answering,
			const std::function<bool(std::string_view)> &receiver,
			long connect_timeout,
			long unacknowledged_timeout);

		feed(const feed &) = delete;

		feed &operator=(const feed &) = delete;

		// Until the server ends the answer, the connection fails, the receiver answers false or
		// stopping is asked for.
		void run(const std::stop_token &stop);

	private:
		void connect();

		void write();

		void read_header();

		void read_body();

		bool hand_on(size_t taken);

		void complete();

		void end(const std::string &why);

		void fail(const boost::system::error_code &error);
	};
}
