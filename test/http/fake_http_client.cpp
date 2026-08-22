#include "fake_http_client.h"

void http::fake_client::answer(const std::string &url, const response &response)
{
	answers.push_back(std::pair<std::string, http::response>(url, response));
}

http::response http::fake_client::send(const request &request) const
{
	std::lock_guard<std::mutex> lock(mutex);

	requests.push_back(request);

	for (size_t i = 0; i < answers.size(); i++)
	{
		if (request.url.find(answers[i].first) != std::string::npos)
		{
			return answers[i].second;
		}
	}

	// Nothing was said about this URL, so it is a node that is not there.
	response response;

	response.message = "Nothing answers at " + request.url;

	return response;
}

std::vector<http::request> http::fake_client::sent() const
{
	std::lock_guard<std::mutex> lock(mutex);

	return requests;
}

std::vector<http::request> http::fake_client::sent_to(const std::string &url) const
{
	std::vector<request> all = sent();
	std::vector<request> matching;

	for (size_t i = 0; i < all.size(); i++)
	{
		if (all[i].url.find(url) != std::string::npos)
		{
			matching.push_back(all[i]);
		}
	}

	return matching;
}

http::response http::answer(long status, const std::string &content_type, const std::string &body)
{
	response response;

	response.is_valid = true;
	response.status = status;
	response.content_type = content_type;
	response.body = body;

	return response;
}
