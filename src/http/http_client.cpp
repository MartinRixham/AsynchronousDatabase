#include <curl/curl.h>

#include "log.h"
#include "http_client.h"

namespace
{
	size_t write_body(void *contents, size_t size, size_t count, void *body)
	{
		static_cast<std::string *>(body)->append(static_cast<const char *>(contents), size * count);

		return size * count;
	}

	// One handle for the life of a thread, because a handle is what holds open connections: a node
	// talks to the same few neighbours over and over, and a new handle each time would mean a new
	// connection each time. A handle belongs to one thread at a time, and this one never leaves the
	// thread it was made on.
	class handle
	{
		CURL *easy;

	public:
		handle():
			easy(curl_easy_init())
		{
		}

		~handle()
		{
			if (easy != NULL)
			{
				curl_easy_cleanup(easy);
			}
		}

		handle(const handle &) = delete;

		handle &operator=(const handle &) = delete;

		CURL *get() const
		{
			return easy;
		}
	};

	// Resetting forgets the options of the request before — a body, or the "no body" of a HEAD,
	// would otherwise be carried into the next one — and keeps the connections, the name lookups
	// and the TLS sessions the handle has already made.
	CURL *thread_handle()
	{
		thread_local class handle handle;

		if (handle.get() != NULL)
		{
			curl_easy_reset(handle.get());
		}

		return handle.get();
	}
}

http::curl_client::curl_client(long timeout):
	timeout_seconds(timeout)
{
}

// libcurl is initialised once by the process — main() and the test binary both do it — because
// curl_global_init is not itself safe to race.
http::response http::curl_client::send(const request &request) const
{
	CURL *curl = thread_handle();
	response response;

	if (curl == NULL)
	{
		response.message = "Failed to create a curl handle.";

		return response;
	}

	struct curl_slist *headers = NULL;

	for (size_t i = 0; i < request.headers.size(); i++)
	{
		headers = curl_slist_append(headers, request.headers[i].c_str());
	}

	// A body large enough to be worth a handshake would otherwise wait for a 100 Continue that
	// this API never sends.
	headers = curl_slist_append(headers, "Expect:");

	curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_seconds);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	if (request.method == "HEAD")
	{
		curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	}
	else if (!request.body.empty() || request.method == "PUT" || request.method == "POST")
	{
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
	}

	CURLcode code = curl_easy_perform(curl);

	if (code == CURLE_OK)
	{
		char *content_type = NULL;
		long connects = 0;

		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
		curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);

		// No connection made is a connection that was already there.
		curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &connects);

		response.content_type = content_type == NULL ? "" : content_type;
		response.reused = connects == 0;
		response.is_valid = true;
	}
	else
	{
		response.message = curl_easy_strerror(code);

		DEBUG("Request to " + request.url + " failed: " + response.message);
	}

	// The handle outlives this list, so it is told to forget the list before the list goes.
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
	curl_slist_free_all(headers);

	return response;
}
