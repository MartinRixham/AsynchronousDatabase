#include <algorithm>
#include <cstring>

#include <boost/json.hpp>
#include <boost/lexical_cast/try_lexical_convert.hpp>

#include "url/url.h"
#include "scan.h"

namespace
{
	constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	std::string encode_base64(const std::string &text)
	{
		std::string encoded;

		for (size_t i = 0; i < text.size(); i += 3)
		{
			size_t remaining = text.size() - i;
			unsigned int group = static_cast<unsigned char>(text[i]) << 16;

			group |= (remaining > 1 ? static_cast<unsigned char>(text[i + 1]) : 0) << 8;
			group |= remaining > 2 ? static_cast<unsigned char>(text[i + 2]) : 0;

			encoded += alphabet[(group >> 18) & 0x3f];
			encoded += alphabet[(group >> 12) & 0x3f];
			encoded += remaining > 1 ? alphabet[(group >> 6) & 0x3f] : '=';
			encoded += remaining > 2 ? alphabet[group & 0x3f] : '=';
		}

		return encoded;
	}

	bool decode_base64(const std::string &encoded, std::string *text)
	{
		unsigned int group = 0;
		int bits = 0;

		for (size_t i = 0; i < encoded.size(); i++)
		{
			if (encoded[i] == '=')
			{
				break;
			}

			const char *found = strchr(alphabet, encoded[i]);

			if (found == NULL || encoded[i] == '\0')
			{
				return false;
			}

			group = (group << 6) | static_cast<unsigned int>(found - alphabet);
			bits += 6;

			if (bits >= 8)
			{
				bits -= 8;
				*text += static_cast<char>((group >> bits) & 0xff);
			}
		}

		return true;
	}

	// The least string that is strictly greater than every key beginning with the prefix. A
	// prefix of nothing but 0xff bytes has none, and the range it names runs to the end.
	bool prefix_end(const std::string &prefix, std::string *end)
	{
		*end = prefix;

		while (!end->empty() && static_cast<unsigned char>(end->back()) == 0xff)
		{
			end->pop_back();
		}

		if (end->empty())
		{
			return false;
		}

		end->back()++;

		return true;
	}

	size_t read_limit(const std::string &query)
	{
		size_t limit = 0;

		// A limit that is not a number at all, or is not only a number, is no limit and the default
		// stands. One that is a number is held between one and the most a single response may hold.
		if (!boost::conversion::try_lexical_convert(url::read_parameter(query, "limit"), limit))
		{
			return scan::default_limit;
		}

		return std::min(std::max(limit, static_cast<size_t>(1)), scan::max_limit);
	}

	// A cursor is opaque, and the instance that issued it is part of what it says: a cursor from
	// another instance names a position in an iteration this one never started.
	bool read_cursor(const std::string &cursor, const std::string &instance, std::string *key)
	{
		std::string decoded;

		if (!decode_base64(cursor, &decoded))
		{
			return false;
		}

		boost::system::error_code error;
		boost::json::value value = boost::json::parse(decoded, error);

		if (error || !value.is_object())
		{
			return false;
		}

		boost::json::object object = value.as_object();

		if (!object.contains("k") || !object["k"].is_string() ||
			!object.contains("s") || !object["s"].is_string() ||
			object["s"].as_string() != instance)
		{
			return false;
		}

		*key = std::string(object["k"].as_string());

		return true;
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
		range.from = prefix;
		range.has_from = true;
		range.has_to = prefix_end(prefix, &range.to);
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
		std::string key;

		if (!read_cursor(cursor, instance, &key))
		{
			return invalid_range("invalid_cursor", "Cursor was not issued by this instance.");
		}

		// Resuming is strictly after the last key returned, and appending a zero byte names the
		// least key above it. Backwards, the last key returned is the exclusive end.
		if (range.reverse)
		{
			range.to = key;
			range.has_to = true;
		}
		else
		{
			range.from = key + std::string(1, '\0');
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

	return encode_base64(boost::json::serialize(cursor));
}
