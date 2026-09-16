#include "handle.h"

http::handle::handle() noexcept:
	easy(curl_easy_init())
{
}

http::handle::~handle()
{
	if (easy != nullptr)
	{
		curl_easy_cleanup(easy);
	}
}

CURL *http::handle::get() const noexcept
{
	return easy;
}
