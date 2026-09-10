#ifndef RECORD_RECORD_H
#define RECORD_RECORD_H

#include <cstddef>
#include <string>
#include <string_view>

namespace record
{
	// Keys live in indexes and bloom filters, which are held in memory, and a value is read whole
	// into memory to be served.
	constexpr size_t max_key_size = 4 * 1024;

	constexpr size_t max_value_size = 16 * 1024 * 1024;

	// A key is a partition key and a sort key, and this is what separates them. A zero byte sorts
	// below every other one, so a partition key's records are together in the store and in sort key
	// order within it. It is the *first* zero byte that separates, which is what keeps the encoding
	// injective and what makes a key carrying one its two halves rather than a partition key of its
	// own.
	constexpr char sort_separator = '\0';

	// The two halves as the store holds them, and back again. An empty sort key adds nothing, so a
	// key of one part is those bytes and no more.
	std::string compose_key(const std::string &partition, const std::string &sort);

	// Views of the key handed in, which has to outlive them.
	std::string_view partition_key(const std::string &key);

	std::string_view sort_key(const std::string &key);

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
