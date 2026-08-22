#include <boost/locale/utf.hpp>

#include "record.h"

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

// Decoding is the check: the traits reject a truncated sequence, a code point above the range, a
// surrogate, and an overlong encoding, which would otherwise be a second spelling of a key.
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
