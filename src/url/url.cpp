#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <curl/curl.h>

#include "url.h"

// A path segment or a query value is escaped whole, so that a slash, a question mark or a zero
// byte inside a key travels as text rather than as punctuation of the URL.
std::string url::encode(const std::string &text)
{
	char *encoded = curl_easy_escape(NULL, text.c_str(), static_cast<int>(text.size()));
	std::string out(encoded);

	curl_free(encoded);

	return out;
}

std::string url::decode(const std::string &encoded)
{
	int length = 0;
	char *decoded = curl_easy_unescape(NULL, encoded.c_str(), static_cast<int>(encoded.size()), &length);

	// A key may contain a zero byte, which is the usual separator inside a composite key, so the
	// length curl reports is what bounds the string rather than the first zero in it.
	std::string out(decoded, static_cast<size_t>(length));

	curl_free(decoded);

	return out;
}

// A path segment ends at an unencoded slash, so the target is split before it is decoded. Decoding
// first would let a key containing "/" or "?" pretend to be a route of its own.
std::vector<std::string> url::split_path(const std::string &target)
{
	std::string path = target.substr(0, target.find('?'));
	std::vector<std::string> segments;
	std::vector<std::string> parts;

	boost::algorithm::split(parts, path, boost::algorithm::is_any_of("/"));

	for (size_t i = 0; i < parts.size(); i++)
	{
		// The empty segment before the leading slash is not a segment, and neither is the one a
		// trailing slash leaves behind: "/table/account/key/" addresses the range, not a key.
		if (!parts[i].empty())
		{
			segments.push_back(decode(parts[i]));
		}
	}

	return segments;
}

std::string url::query_string(const std::string &target)
{
	size_t question_mark = target.find('?');

	if (question_mark == std::string::npos)
	{
		return "";
	}

	return target.substr(question_mark + 1);
}

std::string url::read_parameter(const std::string &query, const std::string &name)
{
	std::vector<std::string> parameters;

	boost::algorithm::split(parameters, query, boost::algorithm::is_any_of("&"));

	for (size_t i = 0; i < parameters.size(); i++)
	{
		size_t equals = parameters[i].find('=');

		if (equals != std::string::npos && parameters[i].substr(0, equals) == name)
		{
			return decode(parameters[i].substr(equals + 1));
		}
	}

	return "";
}
