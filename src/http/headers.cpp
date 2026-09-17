#include <algorithm>
#include <cstring>
#include <iterator>
#include <ranges>
#include <strings.h>

#include <boost/lexical_cast/try_lexical_convert.hpp>

#include "headers.h"

namespace
{
	bool is_ours(const boost::beast::http::fields::value_type &field)
	{
		std::string_view name(field.name_string().data(), field.name_string().size());

		return name.size() >= std::strlen(http::header_prefix) &&
			strncasecmp(name.data(), http::header_prefix, std::strlen(http::header_prefix)) == 0;
	}

	std::pair<std::string, std::string> named(const boost::beast::http::fields::value_type &field)
	{
		return std::pair<std::string, std::string>(
			std::string(field.name_string().data(), field.name_string().size()),
			std::string(field.value().data(), field.value().size()));
	}
}

void http::take_headers(const boost::beast::http::response_header<> &received, response &answer)
{
	answer.status = received.result_int();

	boost::beast::string_view type = received[boost::beast::http::field::content_type];
	boost::beast::string_view length = received[boost::beast::http::field::content_length];

	answer.content_type = std::string(type.data(), type.size());

	// An answer that carried no length at all says nothing about how large the body is, which is
	// a length of none rather than one to guess at.
	answer.content_length = 0;

	boost::conversion::try_lexical_convert(std::string(length.data(), length.size()), answer.content_length);

	answer.headers.clear();

	std::ranges::copy(
		received | std::views::filter(is_ours) | std::views::transform(named),
		std::back_inserter(answer.headers));
}
