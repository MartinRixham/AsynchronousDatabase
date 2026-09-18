#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "cluster/forwarder.h"

namespace cluster
{
	// Nodes that are not there: each of them answers what the test told it to answer, and what it
	// was asked is kept so that a test can say what travelled.
	class fake_forwarder : public forwarder
	{
		std::map<std::string, router::response> answers;

		// The answers a node gives one after another, and how many of them it has given.
		std::map<std::string, std::vector<router::response>> answer_list;

		mutable std::map<std::string, size_t> answered;

		// How long a node takes to answer, which is what a pass runs out of time inside.
		std::map<std::string, std::chrono::milliseconds> delays;

		mutable std::vector<std::pair<std::string, router::request>> requests;

		// A walk asks several nodes at once, so what it was asked and what it has answered are
		// written from several threads. Held by pointer because a fixture hands one of these back
		// by value.
		std::shared_ptr<std::mutex> mutex;

	public:
		fake_forwarder();

		void answer(const std::string &node, const router::response &response);

		// The answers a node gives in turn rather than one answer to everything, so that a caller
		// paging through a scan is answered a page at a time. The last of them answers everything
		// after it, which is a range that stays exhausted.
		void answer_in_turn(const std::string &node, const std::vector<router::response> &responses);

		// A node that takes a while to answer whatever it answers.
		void slow(const std::string &node, std::chrono::milliseconds delay);

		router::response forward(const std::string &node, const router::request &request) const override;

		std::vector<router::response> forward_all(
			const std::vector<std::string> &nodes,
			const router::request &request) const override;

		std::vector<router::response> forward_each(const std::vector<enquiry> &enquiries) const override;

		boost::asio::awaitable<router::response> async_forward(const std::string &node, const router::request &request)
			const override;

		const std::vector<std::pair<std::string, router::request>> &sent() const;

		void forget();
	};
}
