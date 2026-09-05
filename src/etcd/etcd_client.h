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
	// The answer to a claim on a key: who holds it, and the revision it was created at. The claim
	// was this caller's when it was the one that created it.
	struct claim
	{
		bool held = false;

		std::string holder;

		int64_t revision = 0;
	};

	// etcd speaks gRPC, but every call it offers is also a POST of a JSON document to its gateway,
	// where a key and a value travel base64 encoded. That is the whole reason there is no gRPC
	// dependency here: libcurl and Boost.JSON are already in the build.
	//
	// Every member of an etcd cluster answers for the whole of it, so the client is given all of
	// them and asks the next one when the one it was using does not answer. One thread drives a
	// client: the membership of a cluster is kept up by a thread of its own.
	class client
	{
		const http::client &http_client;

		std::vector<std::string> endpoints;

		// The member that answered last, so that a client which has already found a live member
		// does not walk the dead ones again on every call.
		mutable size_t current = 0;

	public:
		client(const http::client &http, const std::vector<std::string> &endpoints);

		// A lease is what makes membership expire: the node's key is written with it, and the key
		// is gone TTL seconds after the node stops saying it is alive.
		std::optional<int64_t> grant_lease(int64_t ttl_seconds) const;

		// False when the lease is not there any more, which is a node that was away long enough
		// to be dropped and has to register again.
		bool keep_alive(int64_t lease) const;

		bool put(const std::string &key, const std::string &value, int64_t lease) const;

		// Writes the key only if nothing holds it, which is how a node is elected: the one whose
		// write created the key leads, and the others are told who did. The revision the key was
		// created at comes back with it, and it is the term — etcd's revisions only ever rise, so
		// a later leader of the same partition always has a higher one than the leader before it.
		//
		// Nothing at all is a call that failed, which is neither a claim won nor one lost.
		std::optional<claim> create(const std::string &key, const std::string &value, int64_t lease) const;

		// Every key under the prefix, with its value, in etcd's key order.
		std::map<std::string, std::string> range(const std::string &prefix) const;

		// Best effort, and asked of one member rather than of every one in turn: a node that is
		// shutting down has a moment to do it in, and a lease nobody revokes runs out by itself.
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
