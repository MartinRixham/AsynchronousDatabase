#include <numeric>

#include <boost/locale/utf.hpp>

#include "record.h"

std::string record::compose_key(const std::string &partition, const std::string &sort)
{
	return sort.empty() ? partition : partition + sort_separator + sort;
}

std::string_view record::partition_key(const std::string &key)
{
	return std::string_view(key).substr(0, key.find(sort_separator));
}

std::string_view record::sort_key(const std::string &key)
{
	size_t separator = key.find(sort_separator);

	return separator == std::string::npos ? std::string_view() : std::string_view(key).substr(separator + 1);
}

namespace
{
	// Eight bytes, most significant first, which is what makes a byte comparison of two of them
	// the comparison of the numbers.
	void append_big_endian(std::string *bytes, uint64_t number)
	{
		for (size_t i = 0; i < 8; i++)
		{
			bytes->push_back(static_cast<char>((number >> (8 * (7 - i))) & 0xff));
		}
	}

	uint64_t read_big_endian(std::string_view bytes)
	{
		return std::accumulate(
			bytes.begin(),
			bytes.begin() + 8,
			static_cast<uint64_t>(0),
			[](uint64_t number, char byte) { return (number << 8) | static_cast<unsigned char>(byte); });
	}
}

std::string record::compose_value(const version &stamp, const std::string &value)
{
	std::string stored;

	stored.reserve(version_size + value.size());

	append_big_endian(&stored, stamp.term);
	append_big_endian(&stored, stamp.count);

	return stored + value;
}

record::version record::version_of(std::string_view stored)
{
	if (stored.size() < version_size)
	{
		return version();
	}

	return version { read_big_endian(stored), read_big_endian(stored.substr(8)) };
}

std::string_view record::value_of(std::string_view stored)
{
	return stored.size() < version_size ? std::string_view() : stored.substr(version_size);
}

bool record::is_newer(std::string_view stored, std::string_view than)
{
	if (stored.size() < version_size)
	{
		return false;
	}

	if (than.size() < version_size)
	{
		return true;
	}

	return stored.substr(0, version_size).compare(than.substr(0, version_size)) > 0;
}

record::record record::parse_key(const std::string &key)
{
	if (!is_valid_utf8(key))
	{
		return invalid_record("invalid_key_encoding", "Key does not percent decode to valid UTF-8.");
	}

	if (key.size() > max_key_size)
	{
		return invalid_record("key_too_large", "Key is longer than " + std::to_string(max_key_size) + " bytes.");
	}

	return valid_record(key, "");
}

record::record record::parse_record(const std::string &key, const std::string &value)
{
	record parsed_key = parse_key(key);

	if (!parsed_key.is_valid)
	{
		return parsed_key;
	}

	if (value.size() > max_value_size)
	{
		return invalid_record("value_too_large", "Value is longer than " + std::to_string(max_value_size) + " bytes.");
	}

	return valid_record(key, value);
}

record::record record::valid_record(const std::string &key, const std::string &value)
{
	return { true, key, value, "", "" };
}

record::record record::invalid_record(const std::string &code, const std::string &message)
{
	return { false, "", "", code, message };
}

bool record::is_valid_utf8(const std::string &text)
{
	std::string::const_iterator character = text.begin();

	while (character != text.end())
	{
		boost::locale::utf::code_point point =
			boost::locale::utf::utf_traits<char>::decode(character, text.end());

		if (!boost::locale::utf::is_valid_codepoint(point))
		{
			return false;
		}
	}

	return true;
}
