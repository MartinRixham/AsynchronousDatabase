#include <algorithm>
#include <iterator>

#include "fake_http_client.h"

void http::fake_client::answer(const std::string &url, const response &response)
{
	std::lock_guard<std::mutex> lock(mutex);

	answers.push_back(reply { url, "", response });
}

void http::fake_client::answer(const std::string &url, const std::string &containing, const response &response)
{
	std::lock_guard<std::mutex> lock(mutex);

	answers.push_back(reply { url, containing, response });
}

void http::fake_client::forget(const std::string &url)
{
	std::lock_guard<std::mutex> lock(mutex);

	std::erase_if(answers, [&url](const reply &answered) { return answered.url == url; });
}

http::response http::fake_client::send(const request &request, long) const
{
	std::lock_guard<std::mutex> lock(mutex);

	requests.push_back(request);

	auto canned = std::ranges::find_if(
		answers,
		[&request](const reply &told)
		{
			return request.url.find(told.url) != std::string::npos &&
				(told.body.empty() || request.body.find(told.body) != std::string::npos);
		});

	if (canned != answers.end())
	{
		return canned->answer;
	}

	// Nothing was said about this URL, so it is a node that is not there.
	response response;

	response.message = "Nothing answers at " + request.url;

	return response;
}

// A fake with nothing to run at once runs them one after another.
std::vector<http::response> http::fake_client::send_all(const std::vector<request> &request_list, long timeout_seconds)
	const
{
	std::vector<response> responses;

	std::ranges::transform(
		request_list,
		std::back_inserter(responses),
		[this, timeout_seconds](const request &each) { return send(each, timeout_seconds); });

	return responses;
}

void http::fake_client::answer_stream(const std::string &url)
{
	std::lock_guard<std::mutex> lock(mutex);

	streams[url];
}

void http::fake_client::push(const std::string &url, const std::string &piece)
{
	{
		std::lock_guard<std::mutex> lock(mutex);

		streams[url].push_back(piece);
	}

	pushed.notify_all();
}

http::response http::fake_client::stream(
	const request &request,
	const std::function<bool(std::string_view)> &receive,
	const std::stop_token &stop) const
{
	std::unique_lock<std::mutex> lock(mutex);

	requests.push_back(request);

	auto streaming = std::ranges::find_if(
		streams,
		[&request](const auto &told) { return request.url.find(told.first) != std::string::npos; });

	if (streaming == streams.end())
	{
		response response;

		response.message = "Nothing answers at " + request.url;

		return response;
	}

	std::deque<std::string> &waiting = streaming->second;

	while (pushed.wait(lock, stop, [&waiting]() { return !waiting.empty(); }))
	{
		std::string piece = waiting.front();

		waiting.pop_front();

		lock.unlock();

		bool going = receive(piece);

		lock.lock();

		if (!going)
		{
			break;
		}
	}

	return http::answer(200, "application/json", "");
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

	std::ranges::copy_if(
		all,
		std::back_inserter(matching),
		[&url](const request &call) { return call.url.find(url) != std::string::npos; });

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
