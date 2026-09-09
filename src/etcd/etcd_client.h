#ifndef ETCD_ETCD_CLIENT_H
#define ETCD_ETCD_CLIENT_H

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
	// here. One thread drives a client.
	class client
	{
		const http::client &http_client;

		std::vector<std::string> endpoints;

		mutable size_t current = 0;

		long timeout_seconds;

	public:
		client(const http::client &http, const std::vector<std::string> &endpoints, long timeout_seconds = 5);

		// A lease is what makes membership expire: the node's key is written with it, and the key
		// is gone TTL seconds after the node stops saying it is alive.
		std::optional<int64_t> grant_lease(int64_t ttl_seconds) const;

		bool keep_alive(int64_t lease) const;

		bool put(const std::string &key, const std::string &value, int64_t lease) const;

		std::optional<claim> create(const std::string &key, const std::string &value, int64_t lease) const;

		std::map<std::string, std::string> range(const std::string &prefix) const;

		bool revoke(int64_t lease) const;

		// The member the next call will be made to.
		const std::string &endpoint() const;

	private:
		std::optional<boost::json::object> call(
			const std::string &method,
			const boost::json::object &body,
			bool every_member) const;
	};
}

#endif
