#ifndef HTTP_FAKE_HTTP_CLIENT_H
#define HTTP_FAKE_HTTP_CLIENT_H

#include <mutex>
#include <string>
#include <vector>

#include "http/http_client.h"

namespace http
{
	// Answers what it was told to answer to a URL holding a given piece of text, and remembers
	// every request it was sent. The membership of a cluster is kept up by a thread of its own,
	// so both are guarded.
	class fake_client : public client
	{
		struct reply
		{
			std::string url;

			// The piece of the request body this answer is for, and nothing when it answers
			// whatever the body is.
			std::string body;

			response answer;
		};

		mutable std::mutex mutex;

		mutable std::vector<request> requests;

		std::vector<reply> answers;

	public:
		void answer(const std::string &url, const response &response);

		// The same for a URL that is asked more than one thing: etcd's gateway is one URL for
		// every range, and the key in the body is what tells them apart. The first answer that
		// matches is the one given, so the one asking for a body goes in first.
		void answer(const std::string &url, const std::string &containing, const response &response);

		response send(const request &request, long timeout_seconds) const override;

		std::vector<response> send_all(const std::vector<request> &request_list, long timeout_seconds) const override;

		std::vector<request> sent() const;

		std::vector<request> sent_to(const std::string &url) const;
	};

	response answer(long status, const std::string &content_type, const std::string &body);

	// A HEAD as a node answers one: the headers of the body and none of the body.
	response head_answer(long status, const std::string &content_type, long content_length);
}

#endif
