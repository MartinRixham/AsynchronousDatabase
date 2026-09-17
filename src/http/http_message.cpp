#include <boost/beast/core/span.hpp>

#include "http_message.h"

namespace
{
	// A line the caller wrote, as the name and the value Beast holds them as. A line with nothing
	// to separate the two is no header at all.
	void set_header(boost::beast::http::fields &fields, const std::string &line)
	{
		size_t colon = line.find(':');

		if (colon == std::string::npos || colon == 0)
		{
			return;
		}

		size_t start = line.find_first_not_of(" \t", colon + 1);

		fields.set(
			line.substr(0, colon),
			start == std::string::npos ? "" : line.substr(start, line.find_last_not_of(" \t") + 1 - start));
	}
}

http::message http::message_of(const location &where, const request &sending)
{
	message carried;
	boost::beast::http::verb method = boost::beast::http::string_to_verb(sending.method);

	if (method == boost::beast::http::verb::unknown)
	{
		carried.method_string(sending.method);
	}
	else
	{
		carried.method(method);
	}

	carried.target(where.target);
	carried.version(11);
	carried.set(boost::beast::http::field::host, where.authority);
	carried.body() = boost::beast::span<const char>(sending.body.data(), sending.body.size());

	for (const auto &line : sending.headers)
	{
		set_header(carried, line);
	}

	// A body large enough to be worth a handshake would otherwise wait for a 100 Continue that
	// only the proxy in front of a node ever answers, so nothing here asks for one.
	carried.prepare_payload();

	return carried;
}
