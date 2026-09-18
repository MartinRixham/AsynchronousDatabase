#include <algorithm>
#include <iterator>
#include <thread>

#include "fake_forwarder.h"

cluster::fake_forwarder::fake_forwarder():
	mutex(std::make_shared<std::mutex>())
{
}

void cluster::fake_forwarder::answer(const std::string &node, const router::response &response)
{
	answers[node] = response;
}

void cluster::fake_forwarder::answer_in_turn(
	const std::string &node,
	const std::vector<router::response> &responses)
{
	answer_list[node] = responses;
}

void cluster::fake_forwarder::slow(const std::string &node, std::chrono::milliseconds delay)
{
	delays[node] = delay;
}

// The answer is settled under the lock and the wait is not, so a node that takes a while to answer
// takes a while to answer every caller rather than holding the others out of the forwarder.
router::response cluster::fake_forwarder::forward(const std::string &node, const router::request &request) const
{
	std::chrono::milliseconds waiting(0);
	router::response given = router::empty_response(boost::beast::http::status::no_content);

	{
		std::lock_guard<std::mutex> lock(*mutex);

		requests.push_back(std::pair<std::string, router::request>(node, request));

		auto waits = delays.find(node);

		if (waits != delays.end())
		{
			waiting = waits->second;
		}

		auto in_turn = answer_list.find(node);

		if (in_turn != answer_list.end() && !in_turn->second.empty())
		{
			size_t taken = answered[node];

			answered[node] = taken + 1;

			given = in_turn->second[std::min(taken, in_turn->second.size() - 1)];
		}
		else
		{
			auto answered_once = answers.find(node);

			if (answered_once != answers.end())
			{
				given = answered_once->second;
			}
		}
	}

	if (waiting.count() > 0)
	{
		std::this_thread::sleep_for(waiting);
	}

	return given;
}

// A fake with nothing to ask at once asks them one after another.
std::vector<router::response> cluster::fake_forwarder::forward_all(
	const std::vector<std::string> &nodes,
	const router::request &request) const
{
	std::vector<router::response> responses;

	std::ranges::transform(
		nodes,
		std::back_inserter(responses),
		[this, &request](const std::string &node) { return forward(node, request); });

	return responses;
}

// One after another rather than at once. What a fan out is for is the time it saves, and there is
// none of that to save here.
std::vector<router::response> cluster::fake_forwarder::forward_each(const std::vector<enquiry> &enquiries) const
{
	std::vector<router::response> responses;

	std::ranges::transform(
		enquiries,
		std::back_inserter(responses),
		[this](const enquiry &asked) { return forward(asked.node, asked.request); });

	return responses;
}

const std::vector<std::pair<std::string, router::request>> &cluster::fake_forwarder::sent() const
{
	return requests;
}

void cluster::fake_forwarder::forget()
{
	requests.clear();
}
