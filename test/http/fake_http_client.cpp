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

http::response http::fake_client::send(const request &request, long timeout_override) const
{
	return send(request);
}

// A fake with nothing to run at once runs them one after another.
std::vector<http::response> http::fake_client::send_all(const std::vector<request> &request_list) const
{
	std::vector<response> responses;

	for (size_t i = 0; i < request_list.size(); i++)
	{
		responses.push_back(send(request_list[i]));
	}

	return responses;
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
	response.content_length = static_cast<long>(body.size());

	return response;
}

http::response http::head_answer(long status, const std::string &content_type, long content_length)
{
	response response;

	response.is_valid = true;
	response.status = status;
	response.content_type = content_type;
	response.content_length = content_length;

	return response;
}
