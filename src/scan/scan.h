#ifndef SCAN_SCAN_H
#define SCAN_SCAN_H

#include <cstddef>
#include <string>
#include <vector>

#include "record/record.h"

namespace scan
{
	constexpr size_t default_limit = 100;

	constexpr size_t max_limit = 1000;

	// A page is bounded in bytes as well as in records, because a limit of a thousand says nothing
	// about their size. A value may be 16 MiB, so a thousand of the largest legal records is a
	// sixteen gigabyte response built in memory on the node answering — an instance that dies
	// before it can write it, is replaced, and comes back empty. The budget ends the page instead,
	// and the cursor it carries is how the client asks for the rest: the same page boundary a
	// limit draws, drawn for a different reason.
	//
	// It is half the largest value rather than the whole of one, so that a page is either several
	// small records or a single large one. A record bigger than the whole budget is still returned,
	// alone, because a scan that could not get past it would never advance.
	constexpr size_t max_page_bytes = 8 * 1024 * 1024;

	// "from" is inclusive and "to" exclusive, which is RocksDB's own convention and the one that
	// makes ranges compose: the "to" of one page is the "from" of the next.
	struct range
	{
		bool is_valid = false;

		std::string code;

		std::string message;

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

	range parse_range(const std::string &query, const std::string &instance);

	range invalid_range(const std::string &code, const std::string &message);

	std::string encode_cursor(const std::string &key, const std::string &instance);
}

#endif
