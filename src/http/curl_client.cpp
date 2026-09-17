#include <cctype>
#include <cstring>
#include <memory>
#include <strings.h>

#include <curl/curl.h>

#include "log.h"
#include "bound_unacknowledged.h"
#include "group.h"
#include "handle.h"
#include "header_list.h"
#include "curl_client.h"

namespace
{
	// Connections kept open per handle, and a handle belongs to one thread. libcurl's own default is
	// a handful, which was under the neighbour count as soon as a cluster grew past six nodes: every
	// forward past the fifth destination a thread had used cost a handshake again. It is a ceiling
	// and not a reservation — a thread holds one to each node it has actually forwarded to.
	constexpr long connection_cache = 64;

	size_t write_body(void *contents, size_t size, size_t count, void *body)
	{
		static_cast<std::string *>(body)->append(static_cast<const char *>(contents), size * count);

		return size * count;
	}

	size_t hand_on(void *contents, size_t size, size_t count, void *receive)
	{
		std::string_view piece(static_cast<const char *>(contents), size * count);

		// Answering less than it was given is how libcurl is told to end the transfer.
		return (*static_cast<std::function<bool(std::string_view)> *>(receive))(piece) ? size * count : 0;
	}

	// Called about once a second while a stream is waiting for more, which is how long stopping one
	// takes.
	int keep_streaming(void *stop, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
	{
		return static_cast<const std::stop_token *>(stop)->stop_requested() ? 1 : 0;
	}

	// Every header of this API's own, kept for the answer they belong to. A status line starts a
	// block of them, so an answer that carried more than one block — a redirect, an interim answer
	// — is read as the last block alone rather than as all of them at once.
	size_t read_header(char *buffer, size_t size, size_t count, void *carried)
	{
		size_t length = size * count;
		std::vector<std::pair<std::string, std::string>> *headers =
			static_cast<std::vector<std::pair<std::string, std::string>> *>(carried);
		std::string line(buffer, length);

		if (line.compare(0, 5, "HTTP/") == 0)
		{
			headers->clear();

			return length;
		}

		size_t colon = line.find(':');

		if (colon == std::string::npos || colon < std::strlen(http::header_prefix))
		{
			return length;
		}

		std::string name = line.substr(0, colon);

		if (strncasecmp(name.c_str(), http::header_prefix, std::strlen(http::header_prefix)) != 0)
		{
			return length;
		}

		size_t start = line.find_first_not_of(" \t", colon + 1);
		size_t end = line.find_last_not_of(" \t\r\n");

		headers->push_back(std::pair<std::string, std::string>(
			name,
			start == std::string::npos || end == std::string::npos || end < start
				? ""
				: line.substr(start, end - start + 1)));

		return length;
	}

	// A socket that refuses the bound is still used, unbounded, rather than failing the request.
	int unacknowledged(void *seconds, curl_socket_t socket, curlsocktype purpose)
	{
		if (purpose == CURLSOCKTYPE_IPCXN)
		{
			http::bound_unacknowledged(socket, *static_cast<long *>(seconds));
		}

		return CURL_SOCKOPT_OK;
	}

	CURL *thread_handle()
	{
		thread_local http::handle handle;

		if (handle.get() != nullptr)
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

	// The options of a request, the same whether it runs on its own or beside others. The headers
	// belong to the caller, and have to last until the transfer has ended.
	std::unique_ptr<http::header_list> apply(
		CURL *curl,
		const http::request &request,
		http::response *response,
		long timeout,
		long connect_timeout,
		const long *unacknowledged_timeout)
	{
		auto headers = std::make_unique<http::header_list>(curl, request.headers);

		curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response->body);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, read_header);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response->headers);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout);
		curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, unacknowledged);
		curl_easy_setopt(curl, CURLOPT_SOCKOPTDATA, unacknowledged_timeout);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

		// A fan out runs in a multi handle, whose own cache is sized from how many transfers were
		// added to it, so it is the handle a request runs on its own that needs telling.
		curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, connection_cache);

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
			char *content_type = nullptr;
			curl_off_t content_length = 0;
			long connects = 0;

			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
			curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);

			// An answer that carried no length at all says nothing about how large the body is,
			// which is a length of none rather than the -1 curl reports it as.
			curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);

			curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &connects);

			response->content_type = content_type == nullptr ? "" : content_type;
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
				code = curl_multi_poll(multi, nullptr, 0, 1000, nullptr);
			}

			if (code != CURLM_OK)
			{
				DEBUG(std::string("A fan out failed: ") + curl_multi_strerror(code));

				break;
			}
		} while (running > 0);

		CURLMsg *message = nullptr;
		int left = 0;

		while ((message = curl_multi_info_read(multi, &left)) != nullptr)
		{
			if (message->msg != CURLMSG_DONE)
			{
				continue;
			}

			char *carried = nullptr;
			char *url = nullptr;

			curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &carried);
			curl_easy_getinfo(message->easy_handle, CURLINFO_EFFECTIVE_URL, &url);

			if (carried != nullptr)
			{
				complete(
					message->easy_handle,
					message->data.result,
					url == nullptr ? "" : url,
					reinterpret_cast<http::response *>(carried));
			}
		}
	}
}

http::curl_client::curl_client(long connect_timeout, long unacknowledged_timeout):
	connect_timeout_seconds(connect_timeout),
	unacknowledged_timeout_seconds(unacknowledged_timeout)
{
}

http::response http::curl_client::send(const request &request, long timeout_seconds) const
{
	CURL *curl = thread_handle();
	response response;

	if (curl == nullptr)
	{
		response.message = "Failed to create a curl handle.";

		return response;
	}

	auto headers = apply(
		curl,
		request,
		&response,
		timeout_seconds,
		connect_timeout_seconds,
		&unacknowledged_timeout_seconds);

	complete(curl, curl_easy_perform(curl), request.url, &response);

	return response;
}

// Every request at once, in one multi handle on this thread, so a node writing a record to its
// copies waits for the slowest rather than for one after another — which is what keeps the thread
// it is serving on free.
std::vector<http::response> http::curl_client::send_all(const std::vector<request> &requests, long timeout_seconds)
	const
{
	std::vector<response> responses(requests.size());

	if (requests.size() < 2)
	{
		if (requests.size() == 1)
		{
			responses[0] = send(requests[0], timeout_seconds);
		}

		return responses;
	}

	http::group &group = thread_group();
	CURLM *multi = group.get();

	if (multi == nullptr)
	{
		for (auto &failed : responses)
		{
			failed.message = "Failed to create a curl multi handle.";
		}

		return responses;
	}

	std::vector<CURL *> handles(requests.size(), nullptr);
	std::vector<std::unique_ptr<header_list>> lists(requests.size());

	for (size_t i = 0; i < requests.size(); i++)
	{
		CURL *easy = group.at(i);

		if (easy == nullptr)
		{
			responses[i].message = "Failed to create a curl handle.";

			continue;
		}

		lists[i] = apply(
			easy,
			requests[i],
			&responses[i],
			timeout_seconds,
			connect_timeout_seconds,
			&unacknowledged_timeout_seconds);

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
		}
	}

	run(multi);

	for (size_t i = 0; i < handles.size(); i++)
	{
		if (handles[i] != nullptr)
		{
			curl_multi_remove_handle(multi, handles[i]);
		}
	}

	return responses;
}

http::response http::curl_client::stream(
	const request &request,
	const std::function<bool(std::string_view)> &receive,
	const std::stop_token &stop) const
{
	CURL *curl = thread_handle();
	response response;

	if (curl == nullptr)
	{
		response.message = "Failed to create a curl handle.";

		return response;
	}

	auto headers = apply(curl, request, &response, 0, connect_timeout_seconds, &unacknowledged_timeout_seconds);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, hand_on);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &receive);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, keep_streaming);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stop);

	// A stream sends nothing once it is asked, so the bound on what goes unacknowledged never comes
	// into it on its own: a probe is something sent, and a server gone from the network is one that
	// acknowledges none of them.
	curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, unacknowledged_timeout_seconds);
	curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, unacknowledged_timeout_seconds);

	complete(curl, curl_easy_perform(curl), request.url, &response);

	return response;
}
