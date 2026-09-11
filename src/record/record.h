#ifndef RECORD_RECORD_H
#define RECORD_RECORD_H

#include <cstddef>
#include <cstdint>
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

	// What orders two writes to one key, so that a pass moving records can tell which of two
	// copies is the later one. **It is not a clock.** The term is the etcd revision that created
	// the claim of the leader that ordered the write, so it rises whenever leadership moves or a
	// leader restarts; the count is that node's own, and rises within a term. It is the node that
	// *orders* a write that stamps it, and one partition is ordered by one node at a time, so no
	// two nodes ever issue the same pair for one key.
	struct version
	{
		uint64_t term = 0;

		uint64_t count = 0;
	};

	// Big endian and fixed width, so the bytes of one sort as the pair does and a store comparing
	// two of them compares bytes rather than decoding either.
	constexpr size_t version_size = 16;

	// The value as the store holds it: the version and then the value itself. Everything the API
	// carries is the value alone, so this is composed on the way in and taken apart on the way
	// out — and a file of records moves the bytes as they are held, which is what carries the
	// version between nodes.
	std::string compose_value(const version &stamp, const std::string &value);

	version version_of(std::string_view stored);

	// A view of the bytes handed in, which have to outlive it.
	std::string_view value_of(std::string_view stored);

	// Whether the first of two stored values was written after the second. Two copies of one write
	// carry one version, which is the pair this cannot order: it is not newer, so a store meeting
	// one keeps what it holds.
	bool is_newer(std::string_view stored, std::string_view than);

	struct record
	{
		bool is_valid = false;

		std::string key;

		std::string value;

		std::string code;

		std::string message;

		// Set by the node that ordered the write and carried to every copy, so that the copies of
		// one write all hold the same one. A record read out of a store carries the one it was
		// written with.
		version stamp;
	};

	record parse_key(const std::string &key);

	record parse_record(const std::string &key, const std::string &value);

	record valid_record(const std::string &key, const std::string &value);

	record invalid_record(const std::string &code, const std::string &message);

	bool is_valid_utf8(const std::string &text);
}

#endif
