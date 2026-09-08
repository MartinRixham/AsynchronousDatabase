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

	// Half the largest value, so a page is either several small records or a single large one.
	constexpr size_t max_page_bytes = 8 * 1024 * 1024;

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
