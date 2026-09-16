#pragma once

#include <curl/curl.h>

namespace http
{
	class handle
	{
		CURL *easy;

	public:
		handle() noexcept;

		~handle();

		handle(const handle &) = delete;

		handle &operator=(const handle &) = delete;

		CURL *get() const noexcept;
	};
}
