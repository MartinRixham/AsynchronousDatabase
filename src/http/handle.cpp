#include "handle.h"

http::handle::handle():
	easy(curl_easy_init())
{
}

http::handle::~handle()
{
	if (easy != NULL)
	{
		curl_easy_cleanup(easy);
	}
}

CURL *http::handle::get() const
{
	return easy;
}
