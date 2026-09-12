#include <algorithm>
#include <optional>

#include <boost/json.hpp>
#include <boost/lexical_cast/try_lexical_convert.hpp>

#include "base64/base64.h"
#include "cluster/partition.h"
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

	std::optional<size_t> partition_named(const std::string &query)
	{
		std::string named = url::read_parameter(query, "partition");
		std::string key = url::read_parameter(query, "key");

		if (named.empty() == key.empty())
		{
			return std::nullopt;
		}

		if (!key.empty())
		{
			return cluster::partition_of(key);
		}

		size_t partition = 0;

		if (!boost::conversion::try_lexical_convert(named, partition) || partition >= cluster::partition_count)
		{
			return std::nullopt;
		}

		return partition;
	}

	std::optional<std::string> read_cursor(const std::string &cursor, const std::string &instance, size_t partition)
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
			object["s"].as_string() != instance ||
			!object.contains("p") || !object["p"].is_int64() ||
			object["p"].as_int64() != static_cast<int64_t>(partition))
		{
			return std::nullopt;
		}

		return std::string(object["k"].as_string());
	}
}

std::optional<size_t> scan::read_partition(const std::string &query)
{
	return partition_named(query);
}

scan::range scan::parse_range(const std::string &query, const std::string &instance)
{
	range range;
	std::optional<size_t> partition = read_partition(query);

	if (!partition)
	{
		return invalid_range(
			"invalid_partition",
			"A scan names the partition it reads, as \"partition\" or as a \"key\" that is in it.");
	}

	range.partition = *partition;

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
		std::optional<std::string> key = read_cursor(cursor, instance, range.partition);

		if (!key)
		{
			return invalid_range("invalid_cursor", "Cursor was not issued by this instance for this partition.");
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

std::string scan::encode_cursor(const std::string &key, const std::string &instance, size_t partition)
{
	boost::json::object cursor {
		{ "k", boost::json::string(key) },
		{ "s", boost::json::string(instance) },
		{ "p", static_cast<int64_t>(partition) }
	};

	return base64::encode(boost::json::serialize(cursor));
}
