#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "http/http_client.h"

namespace etcd
{
	struct claim
	{
		bool held = false;

		std::string holder;

		int64_t revision = 0;
	};

	// etcd speaks gRPC, but every call it offers is also a POST of a JSON document to its gateway,
	// where a key and a value travel base64 encoded — which is why there is no gRPC dependency
	// here. The membership thread makes most of the calls, and a thread answering a request may make
	// them too: the member being asked is atomic, and the curl handles are the thread's own.
	class client
	{
		const http::client &http_client;

		std::vector<std::string> endpoints;

		// Atomic because the thread making the calls writes it and a thread answering a health
		// check reads it.
		mutable std::atomic<size_t> current = 0;

		long timeout_seconds;

	public:
		client(const http::client &http, const std::vector<std::string> &endpoints, long timeout = 5);

		// A lease is what makes membership expire: the node's key is written with it, and the key
		// is gone TTL seconds after the node stops saying it is alive.
		std::optional<int64_t> grant_lease(int64_t ttl_seconds) const;

		[[nodiscard]] bool keep_alive(int64_t lease) const;

		[[nodiscard]] bool put(const std::string &key, const std::string &value, int64_t lease) const;

		[[nodiscard]] std::optional<claim> create(
			const std::string &key,
			const std::string &value,
			int64_t lease) const;

		// Deleting a key only while it still holds the value it is given. That is what makes giving
		// a claim up safe: a claim whose lease ran out between the range that read it and this call
		// belongs to whichever node claimed it next, and that node is leading on it.
		[[nodiscard]] bool remove(const std::string &key, const std::string &value) const;

		// Nothing when etcd did not answer, which is a different answer from no key under the prefix.
		[[nodiscard]] std::optional<std::map<std::string, std::string>> range(const std::string &prefix) const;

		// The one key and nothing under it, which a range cannot ask for: a prefix of
		// "/asyncdb/leader/1" is every partition from 10 to 199 as well.
		[[nodiscard]] std::optional<std::map<std::string, std::string>> get(const std::string &key) const;

		[[nodiscard]] bool revoke(int64_t lease) const;

		// Every key under a prefix, watched until the watch ends: stopped, cut off, or cancelled or
		// refused by etcd. changed is called for each answer the watch carries, and one of them is
		// the watch being created, so a caller that reads the keys again on every call misses no
		// change made before the watch was there. It is made to the member the other calls last
		// reached.
		void watch(const std::string &prefix, const std::function<void()> &changed, const std::stop_token &stop) const;

		// The member the next call will be made to.
		const std::string &endpoint() const;

	private:
		[[nodiscard]] std::optional<std::map<std::string, std::string>> read(const boost::json::object &request) const;

		[[nodiscard]] std::optional<boost::json::object> call(
			const std::string &method,
			const boost::json::object &body,
			bool every_member) const;
	};
}
