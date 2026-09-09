#include <curl/curl.h>

#include "log.h"
#include "group.h"
#include "handle.h"
#include "curl_client.h"

namespace
{
	size_t write_body(void *contents, size_t size, size_t count, void *body)
	{
		static_cast<std::string *>(body)->append(static_cast<const char *>(contents), size * count);

		return size * count;
	}

	CURL *thread_handle()
	{
		thread_local http::handle handle;

		if (handle.get() != NULL)
		{
			curl_easy_reset(handle.get());
		}

		return handle.get();
	}

	http::group &thread_group()
	{
		thread_local http::group group;

		return group;
	}

	// The options of a request, the same whether it runs on its own or beside others. The list of
	// headers belongs to the caller, and the handle has to be told to forget it before it goes.
	struct curl_slist *apply(
		CURL *curl,
		const http::request &request,
		std::string *body,
		long timeout,
		long connect_timeout)
	{
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
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout);
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

		return headers;
	}

	// What a handle has to say once its transfer has ended, on its own or in a multi handle.
	void complete(CURL *curl, CURLcode code, const std::string &url, http::response *response)
	{
		if (code == CURLE_OK)
		{
			char *content_type = NULL;
			curl_off_t content_length = 0;
			long connects = 0;

			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
			curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);

			// An answer that carried no length at all says nothing about how large the body is,
			// which is a length of none rather than the -1 curl reports it as.
			curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);

			curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &connects);

			response->content_type = content_type == NULL ? "" : content_type;
			response->content_length = content_length > 0 ? static_cast<long>(content_length) : 0;
			response->reused = connects == 0;
			response->is_valid = true;

			// A transfer that was waiting to be run says so until it has been, so an answer that
			// arrived clears what it was waiting on.
			response->message.clear();
		}
		else
		{
			response->message = curl_easy_strerror(code);

			DEBUG("Request to " + url + " failed: " + response->message);
		}
	}

	// Runs every transfer in the multi handle until none is still going, and reads each answer off
	// the handle that carried it. One thread waits on all of their sockets at once, so it is held
	// for as long as the slowest takes rather than for the sum of them.
	void run(CURLM *multi)
	{
		int running = 0;

		do
		{
			CURLMcode code = curl_multi_perform(multi, &running);

			if (code == CURLM_OK && running > 0)
			{
				// A poll with nothing to wait on returns rather than blocking, and one that could
				// block for ever is a fan out that never ends, so it is given a bound. Each
				// transfer's own timeout is what ends a node that has stopped answering.
				code = curl_multi_poll(multi, NULL, 0, 1000, NULL);
			}

			if (code != CURLM_OK)
			{
				DEBUG(std::string("A fan out failed: ") + curl_multi_strerror(code));

				break;
			}
		} while (running > 0);

		CURLMsg *message = NULL;
		int left = 0;

		while ((message = curl_multi_info_read(multi, &left)) != NULL)
		{
			if (message->msg != CURLMSG_DONE)
			{
				continue;
			}

			char *carried = NULL;
			char *url = NULL;

			curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &carried);
			curl_easy_getinfo(message->easy_handle, CURLINFO_EFFECTIVE_URL, &url);

			if (carried != NULL)
			{
				complete(
					message->easy_handle,
					message->data.result,
					url == NULL ? "" : url,
					reinterpret_cast<http::response *>(carried));
			}
		}
	}
}

http::curl_client::curl_client(long timeout, long connect_timeout):
	timeout_seconds(timeout),
	connect_timeout_seconds(connect_timeout)
{
}

http::response http::curl_client::send(const request &request) const
{
	return send(request, timeout_seconds);
}

http::response http::curl_client::send(const request &request, long timeout_override) const
{
	CURL *curl = thread_handle();
	response response;

	if (curl == NULL)
	{
		response.message = "Failed to create a curl handle.";

		return response;
	}

	struct curl_slist *headers = apply(curl, request, &response.body, timeout_seconds, connect_timeout_seconds);

	complete(curl, curl_easy_perform(curl), request.url, &response);

	// The handle outlives this list, so it is told to forget the list before the list goes.
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
	curl_slist_free_all(headers);

	return response;
}

// Every request at once, in one multi handle on this thread, so a node writing a record to its
// copies waits for the slowest rather than for one after another — which is what keeps the thread
// it is serving on free.
std::vector<http::response> http::curl_client::send_all(const std::vector<request> &requests) const
{
	std::vector<response> responses(requests.size());

	if (requests.size() < 2)
	{
		if (requests.size() == 1)
		{
			responses[0] = send(requests[0]);
		}

		return responses;
	}

	http::group &group = thread_group();
	CURLM *multi = group.get();

	if (multi == NULL)
	{
		for (size_t i = 0; i < responses.size(); i++)
		{
			responses[i].message = "Failed to create a curl multi handle.";
		}

		return responses;
	}

	std::vector<CURL *> handles(requests.size(), NULL);
	std::vector<struct curl_slist *> lists(requests.size(), NULL);

	for (size_t i = 0; i < requests.size(); i++)
	{
		CURL *easy = group.at(i);

		if (easy == NULL)
		{
			responses[i].message = "Failed to create a curl handle.";

			continue;
		}

		lists[i] = apply(easy, requests[i], &responses[i].body, timeout_seconds, connect_timeout_seconds);

		// The answers are held still for the whole fan out, so a handle can carry a pointer to
		// its own.
		curl_easy_setopt(easy, CURLOPT_PRIVATE, reinterpret_cast<char *>(&responses[i]));

		if (curl_multi_add_handle(multi, easy) == CURLM_OK)
		{
			handles[i] = easy;

			// Overwritten by the answer, and left to stand by a transfer that never ended.
			responses[i].message = "The fan out ended before this request did.";
		}
		else
		{
			responses[i].message = "Failed to add a curl handle to the fan out.";

			// The handle is not going to be run, so it is told to forget the list all the same.
			curl_easy_setopt(easy, CURLOPT_HTTPHEADER, NULL);
			curl_slist_free_all(lists[i]);

			lists[i] = NULL;
		}
	}

	run(multi);

	for (size_t i = 0; i < handles.size(); i++)
	{
		if (handles[i] != NULL)
		{
			curl_multi_remove_handle(multi, handles[i]);

			// The handle outlives this list, so it is told to forget the list before the list goes.
			curl_easy_setopt(handles[i], CURLOPT_HTTPHEADER, NULL);
		}

		curl_slist_free_all(lists[i]);
	}

	return responses;
}
