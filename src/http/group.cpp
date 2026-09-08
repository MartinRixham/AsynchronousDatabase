#include "group.h"

http::group::group():
	multi(curl_multi_init())
{
}

http::group::~group()
{
	// A handle outliving the multi handle it ran in is one left holding a connection of its own,
	// so the handles go first.
	handles.clear();

	if (multi != NULL)
	{
		curl_multi_cleanup(multi);
	}
}

CURLM *http::group::get() const
{
	return multi;
}

CURL *http::group::at(size_t index)
{
	while (handles.size() <= index)
	{
		handles.push_back(std::make_unique<handle>());
	}

	CURL *easy = handles[index]->get();

	if (easy != NULL)
	{
		curl_easy_reset(easy);
	}

	return easy;
}
