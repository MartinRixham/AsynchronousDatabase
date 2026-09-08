#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <curl/curl.h>

#include "url.h"

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

	std::string out(decoded, static_cast<size_t>(length));

	curl_free(decoded);

	return out;
}

std::vector<std::string> url::split_path(const std::string &target)
{
	std::string path = target.substr(0, target.find('?'));
	std::vector<std::string> segments;
	std::vector<std::string> parts;

	boost::algorithm::split(parts, path, boost::algorithm::is_any_of("/"));

	for (size_t i = 0; i < parts.size(); i++)
	{
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
