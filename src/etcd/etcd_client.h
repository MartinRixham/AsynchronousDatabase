#ifndef ETCD_ETCD_CLIENT_H
#define ETCD_ETCD_CLIENT_H

#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
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
	// one too: the member being asked is atomic, and the curl handles are the thread's own.
	class client
	{
		const http::client &http_client;

		std::vector<std::string> endpoints;

		// Atomic because the thread making the calls writes it and a thread answering a health
		// check reads it.
		mutable std::atomic<size_t> current = 0;

		long timeout_seconds;

	public:
		client(const http::client &http, const std::vector<std::string> &endpoints, long timeout_seconds = 5);

		// A lease is what makes membership expire: the node's key is written with it, and the key
		// is gone TTL seconds after the node stops saying it is alive.
		std::optional<int64_t> grant_lease(int64_t ttl_seconds) const;

		bool keep_alive(int64_t lease) const;

		bool put(const std::string &key, const std::string &value, int64_t lease) const;

		std::optional<claim> create(const std::string &key, const std::string &value, int64_t lease) const;

		// Deleting a key only while it still holds the value it is given. That is what makes giving
		// a claim up safe: a claim whose lease ran out between the range that read it and this call
		// belongs to whichever node claimed it next, and that node is leading on it.
		bool remove(const std::string &key, const std::string &value) const;

		// Nothing when etcd did not answer, which is a different answer from no key under the prefix.
		std::optional<std::map<std::string, std::string>> range(const std::string &prefix) const;

		// The one key and nothing under it, which a range cannot ask for: a prefix of
		// "/asyncdb/leader/1" is every partition from 10 to 199 as well.
		std::optional<std::map<std::string, std::string>> get(const std::string &key) const;

		bool revoke(int64_t lease) const;

		// The member the next call will be made to.
		const std::string &endpoint() const;

	private:
		std::optional<std::map<std::string, std::string>> read(const boost::json::object &request) const;

		std::optional<boost::json::object> call(
			const std::string &method,
			const boost::json::object &body,
			bool every_member) const;
	};
}

#endif
