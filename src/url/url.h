#ifndef URL_URL_H
#define URL_URL_H

#include <string>
#include <vector>

namespace url
{
	std::string encode(const std::string &text);

	std::string decode(const std::string &encoded);

	std::vector<std::string> split_path(const std::string &target);

	std::string query_string(const std::string &target);

	std::string read_parameter(const std::string &query, const std::string &name);
}

#endif
