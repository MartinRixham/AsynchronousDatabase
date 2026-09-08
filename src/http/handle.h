#ifndef HTTP_HANDLE_H
#define HTTP_HANDLE_H

#include <curl/curl.h>

namespace http
{
	class handle
	{
		CURL *easy;

	public:
		handle();

		~handle();

		handle(const handle &) = delete;

		handle &operator=(const handle &) = delete;

		CURL *get() const;
	};
}

#endif
