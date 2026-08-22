#ifndef RECORD_RECORD_H
#define RECORD_RECORD_H

#include <cstddef>
#include <string>

namespace record
{
	// Counted in the bytes of the UTF-8 encoding. Keys live in indexes and bloom filters, which
	// are held in memory, and a value is read whole into memory to be served.
	constexpr size_t max_key_size = 4 * 1024;

	constexpr size_t max_value_size = 16 * 1024 * 1024;

	struct record
	{
		bool is_valid = false;

		std::string key;

		std::string value;

		std::string code;

		std::string message;
	};

	record parse_key(const std::string &key);

	record parse_record(const std::string &key, const std::string &value);

	record valid_record(const std::string &key, const std::string &value);

	record invalid_record(const std::string &code, const std::string &message);

	bool is_valid_utf8(const std::string &text);
}

#endif
