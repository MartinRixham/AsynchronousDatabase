#ifndef HTTP_GROUP_H
#define HTTP_GROUP_H

#include <cstddef>
#include <memory>
#include <vector>

#include <curl/curl.h>

#include "handle.h"

namespace http
{
	class group
	{
		CURLM *multi;

		std::vector<std::unique_ptr<handle>> handles;

	public:
		group();

		~group();

		group(const group &) = delete;

		group &operator=(const group &) = delete;

		CURLM *get() const;

		// As many handles as the widest fan out this thread has run, reset rather than remade.
		CURL *at(size_t index);
	};
}

#endif
