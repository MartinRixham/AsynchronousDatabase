#pragma once

#include <string>
#include <vector>

#include <curl/curl.h>

namespace http
{
	// The headers of one request, set on the handle that sends it. The handle keeps a pointer to
	// the list and outlives it, so the list is taken off the handle before it is freed.
	class header_list
	{
		CURL *curl;

		struct curl_slist *list = nullptr;

	public:
		header_list(CURL *handle, const std::vector<std::string> &lines);

		~header_list();

		header_list(const header_list &) = delete;

		header_list &operator=(const header_list &) = delete;
	};
}
