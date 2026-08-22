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
		mutable std::mutex mutex;

		mutable std::vector<request> requests;

		std::vector<std::pair<std::string, response>> answers;

	public:
		void answer(const std::string &url, const response &response);

		response send(const request &request) const override;

		std::vector<request> sent() const;

		std::vector<request> sent_to(const std::string &url) const;
	};

	response answer(long status, const std::string &content_type, const std::string &body);
}

#endif
