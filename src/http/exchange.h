#pragma once

#include <chrono>
#include <optional>

#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>

#include "connection.h"
#include "http_client.h"
#include "http_message.h"
#include "location.h"

namespace http
{
	// One request and the answer to it, run as a chain of handlers on the io_context the
	// connection belongs to. A fan out is several of these started before any of them is waited
	// for, so the thread is held for as long as the slowest takes rather than for the sum of them.
	class exchange
	{
		connection &link;

		const request &sent;

		response &answer;

		long connect_timeout_seconds;

		long unacknowledged_timeout_seconds;

		bool bounded;

		// The whole transfer, connecting included.
		std::chrono::steady_clock::time_point deadline;

		// A connection out of the pool may have been closed by the node at the other end while
		// nothing was going on it, which is a request to make again rather than a request that
		// failed. Only once, and only while nothing of the answer has been read.
		bool redialed = false;

		message carried;

		std::optional<boost::beast::http::response_parser<boost::beast::http::string_body>> parser;

	public:
		exchange(
			connection &held,
			const location &where,
			const request &sending,
			response &answering,
			long timeout_seconds,
			long connect_timeout,
			long unacknowledged_timeout);

		exchange(const exchange &) = delete;

		exchange &operator=(const exchange &) = delete;

		void start();

	private:
		void connect();

		void write();

		void read();

		void complete();

		bool redial();

		void fail(const boost::system::error_code &error);
	};
}
