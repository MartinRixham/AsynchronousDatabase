#include <algorithm>
#include <optional>

#include <boost/json.hpp>
#include <boost/lexical_cast/try_lexical_convert.hpp>

#include "base64/base64.h"
#include "url/url.h"
#include "scan.h"

namespace
{
	std::optional<std::string> prefix_end(const std::string &prefix)
	{
		std::string end = prefix;

		while (!end.empty() && static_cast<unsigned char>(end.back()) == 0xff)
		{
			end.pop_back();
		}

		if (end.empty())
		{
			return std::nullopt;
		}

		end.back()++;

		return end;
	}

	size_t read_limit(const std::string &query)
	{
		size_t limit = 0;

		if (!boost::conversion::try_lexical_convert(url::read_parameter(query, "limit"), limit))
		{
			return scan::default_limit;
		}

		return std::min(std::max(limit, static_cast<size_t>(1)), scan::max_limit);
	}

	std::optional<std::string> read_cursor(const std::string &cursor, const std::string &instance)
	{
		std::optional<std::string> decoded = base64::decode(cursor);

		if (!decoded)
		{
			return std::nullopt;
		}

		boost::system::error_code error;
		boost::json::value value = boost::json::parse(*decoded, error);

		if (error || !value.is_object())
		{
			return std::nullopt;
		}

		boost::json::object object = value.as_object();

		if (!object.contains("k") || !object["k"].is_string() ||
			!object.contains("s") || !object["s"].is_string() ||
			object["s"].as_string() != instance)
		{
			return std::nullopt;
		}

		return std::string(object["k"].as_string());
	}
}

scan::range scan::parse_range(const std::string &query, const std::string &instance)
{
	range range;
	std::string prefix = url::read_parameter(query, "prefix");
	std::string from = url::read_parameter(query, "from");
	std::string to = url::read_parameter(query, "to");

	if (!prefix.empty())
	{
		std::optional<std::string> end = prefix_end(prefix);

		range.from = prefix;
		range.has_from = true;

		if (end)
		{
			range.to = *end;
			range.has_to = true;
		}
	}

	if (!from.empty())
	{
		range.from = from;
		range.has_from = true;
	}

	if (!to.empty())
	{
		range.to = to;
		range.has_to = true;
	}

	if (range.has_from && range.has_to && range.from >= range.to)
	{
		return invalid_range("invalid_range", "Range from \"" + range.from + "\" is not below to \"" + range.to + "\".");
	}

	range.reverse = url::read_parameter(query, "reverse") == "true";
	range.values = url::read_parameter(query, "values") != "false";
	range.limit = read_limit(query);
	range.is_valid = true;

	std::string cursor = url::read_parameter(query, "cursor");

	if (!cursor.empty())
	{
		std::optional<std::string> key = read_cursor(cursor, instance);

		if (!key)
		{
			return invalid_range("invalid_cursor", "Cursor was not issued by this instance.");
		}

		if (range.reverse)
		{
			range.to = *key;
			range.has_to = true;
		}
		else
		{
			range.from = *key + std::string(1, '\0');
			range.has_from = true;
		}
	}

	return range;
}

scan::range scan::invalid_range(const std::string &code, const std::string &message)
{
	range range;

	range.code = code;
	range.message = message;

	return range;
}

std::string scan::encode_cursor(const std::string &key, const std::string &instance)
{
	boost::json::object cursor { { "k", boost::json::string(key) }, { "s", boost::json::string(instance) } };

	return base64::encode(boost::json::serialize(cursor));
}
