#include <numeric>

#include "header_list.h"

namespace
{
	// curl_slist_append answers nothing when it cannot allocate and leaves the list as it was, so
	// the list is kept rather than overwritten with the failure.
	struct curl_slist *append(struct curl_slist *list, const char *line)
	{
		struct curl_slist *appended = curl_slist_append(list, line);

		return appended == nullptr ? list : appended;
	}
}

http::header_list::header_list(CURL *handle, const std::vector<std::string> &lines):
	curl(handle)
{
	list = std::accumulate(
		lines.begin(),
		lines.end(),
		list,
		[](struct curl_slist *appended, const std::string &line) { return append(appended, line.c_str()); });

	// A body large enough to be worth a handshake would otherwise wait for a 100 Continue that
	// this API never sends.
	list = append(list, "Expect:");

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
}

http::header_list::~header_list()
{
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
	curl_slist_free_all(list);
}
