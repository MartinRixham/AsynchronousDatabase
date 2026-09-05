#include <memory>

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

	// The handles a fan out runs on, and the multi handle it runs them in. They belong to the
	// thread the single handle does, and for the same reason: the multi handle is what holds the
	// connections of every transfer run in it, so a node writing to the same copies over and over
	// keeps its connections to them rather than making them again each time.
	class group
	{
		CURLM *multi;

		std::vector<std::unique_ptr<class handle>> handles;

	public:
		group():
			multi(curl_multi_init())
		{
		}

		~group()
		{
			// Every handle is taken out of the multi handle as its fan out ends, so nothing here
			// is still in it. They go first all the same, because a handle outliving the multi
			// handle it ran in is the one that would be left holding a connection of its own.
			handles.clear();

			if (multi != NULL)
			{
				curl_multi_cleanup(multi);
			}
		}

		group(const group &) = delete;

		group &operator=(const group &) = delete;

		CURLM *get() const
		{
			return multi;
		}

		// As many handles as the widest fan out this thread has run, kept from one to the next and
		// reset rather than remade, for the same reason the single handle is.
		CURL *at(size_t index)
		{
			while (handles.size() <= index)
			{
				handles.push_back(std::make_unique<class handle>());
			}

			CURL *easy = handles[index]->get();

			if (easy != NULL)
			{
				curl_easy_reset(easy);
			}

			return easy;
		}
	};

	group &thread_group()
	{
		thread_local class group group;

		return group;
	}

	// The options of a request, which are the same whether it runs on its own or beside others.
	// The list of headers belongs to the caller: the handle outlives it, and has to be told to
	// forget it before it goes.
	struct curl_slist *apply(CURL *curl, const http::request &request, std::string *body, long timeout)
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
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

		if (request.method == "HEAD")
		{
			curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
		}
		// The body is not copied into the handle, so the request it belongs to has to outlive the
		// transfer — which it does: a fan out is run before the requests it was given go.
		else if (!request.body.empty() || request.method == "PUT" || request.method == "POST")
		{
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
		}

		return headers;
	}

	// What a handle has to say once its transfer has ended, whether it ran on its own or in a
	// multi handle beside others.
	void complete(CURL *curl, CURLcode code, const std::string &url, http::response *response)
	{
		if (code == CURLE_OK)
		{
			char *content_type = NULL;
			long connects = 0;

			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
			curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);

			// No connection made is a connection that was already there.
			curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &connects);

			response->content_type = content_type == NULL ? "" : content_type;
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

	// Runs every transfer in the multi handle until none of them is still going, and reads each
	// answer off the handle that carried it. One thread waits on all of their sockets at once,
	// which is the whole point: the thread is held for as long as the slowest of them takes
	// rather than for the sum of them.
	void run(CURLM *multi)
	{
		int running = 0;

		do
		{
			CURLMcode code = curl_multi_perform(multi, &running);

			if (code == CURLM_OK && running > 0)
			{
				// A poll with nothing to wait on returns rather than blocking, and one that could
				// block for ever is a fan out that never ends, so it is given a bound. The timeout
				// of each transfer is what actually ends a node that has stopped answering.
				code = curl_multi_poll(multi, NULL, 0, 1000, NULL);
			}

			if (code != CURLM_OK)
			{
				DEBUG(std::string("A fan out failed: ") + curl_multi_strerror(code));

				break;
			}
		}
		while (running > 0);

		CURLMsg *message = NULL;
		int left = 0;

		while ((message = curl_multi_info_read(multi, &left)) != NULL)
		{
			if (message->msg != CURLMSG_DONE)
			{
				continue;
			}

			// Each handle carries which answer is its own, so the transfers are read back in
			// whatever order they finished in and still answered in the order they were asked.
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

http::curl_client::curl_client(long timeout):
	timeout_seconds(timeout)
{
}

// A client with no way of running several requests at once runs them one after another, which is
// the answer this gives and the order it gives it in.
std::vector<http::response> http::client::send_all(const std::vector<request> &requests) const
{
	std::vector<response> responses;

	for (size_t i = 0; i < requests.size(); i++)
	{
		responses.push_back(send(requests[i]));
	}

	return responses;
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

	struct curl_slist *headers = apply(curl, request, &response.body, timeout_seconds);

	complete(curl, curl_easy_perform(curl), request.url, &response);

	// The handle outlives this list, so it is told to forget the list before the list goes.
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
	curl_slist_free_all(headers);

	return response;
}

// Every request at once, in one multi handle on this thread. A node writing a record to the copies
// of it waits for the slowest of them rather than for one after another, which is what keeps the
// thread it is serving on free: a node has as many of those as it has cores, and a thread waiting
// on a round trip is a thread the next request cannot be served on.
std::vector<http::response> http::curl_client::send_all(const std::vector<request> &requests) const
{
	std::vector<response> responses(requests.size());

	// Nothing to overlap. One request goes on the handle that already holds the connections of
	// every other request this thread has sent on its own.
	if (requests.size() < 2)
	{
		if (requests.size() == 1)
		{
			responses[0] = send(requests[0]);
		}

		return responses;
	}

	class group &group = thread_group();
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

		lists[i] = apply(easy, requests[i], &responses[i].body, timeout_seconds);

		// The answers are held still for the whole fan out — the vector is sized before any of
		// this — so a handle can carry a pointer to its own.
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

			// The handle is not going to be run, so it is told to forget the list before the
			// list goes, the same as one that was.
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
