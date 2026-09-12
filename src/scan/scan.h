#ifndef SCAN_SCAN_H
#define SCAN_SCAN_H

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "record/record.h"

namespace scan
{
	constexpr size_t default_limit = 100;

	constexpr size_t max_limit = 1000;

	// Half the largest value, so a page is either several small records or a single large one.
	constexpr size_t max_page_bytes = 8 * 1024 * 1024;

	struct range
	{
		bool is_valid = false;

		std::string code;

		std::string message;

		// **The partition this scan reads, and a scan reads one.** A key belongs to one of the
		// partitions, the store sorts the partitions apart, and one node of a zone holds each of
		// them — so a range of keys is a range inside a partition, and a walk of a whole table is
		// one scan per partition rather than one scan.
		size_t partition = 0;

		std::string from;

		std::string to;

		bool has_from = false;

		bool has_to = false;

		bool reverse = false;

		bool values = true;

		size_t limit = default_limit;
	};

	struct page
	{
		std::vector<record::record> records;

		bool has_more = false;
	};

	// The partition a scan reads: `partition` names one by number, and `key` names one by a key it
	// holds — a client cannot hash a key itself, and the records of one partition key are what a
	// scan of a partition is usually after. Exactly one of the two, or nothing.
	//
	// **It is read on its own because it is what routes the scan**, and routing comes first: the
	// rest of the range is read by the node that answers, which is the node whose cursor it is.
	std::optional<size_t> read_partition(const std::string &query);

	// The whole of the range, for the node that is going to answer it. A query naming no partition
	// is `invalid_partition` here.
	range parse_range(const std::string &query, const std::string &instance);

	range invalid_range(const std::string &code, const std::string &message);

	// The cursor carries the partition beside the key, so a cursor resumed against a different
	// partition is refused rather than answered with the nothing that key holds there.
	std::string encode_cursor(const std::string &key, const std::string &instance, size_t partition);
}

#endif
