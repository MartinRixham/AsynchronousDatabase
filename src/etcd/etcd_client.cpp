#include <algorithm>

#include <boost/json.hpp>
#include <boost/lexical_cast/try_lexical_convert.hpp>

#include "base64/base64.h"
#include "log.h"
#include "etcd_client.h"

namespace
{
	// etcd names a range by its end, and the end of a prefix is the prefix with its last byte
	// raised. A prefix of nothing but 0xff bytes has no end, and "\0" is how etcd spells "to the
	// end of the keyspace".
	std::string range_end(const std::string &prefix)
	{
		std::string end = prefix;

		while (!end.empty() && static_cast<unsigned char>(end.back()) == 0xff)
		{
			end.pop_back();
		}

		if (end.empty())
		{
			return std::string(1, '\0');
		}

		end.back()++;

		return end;
	}

	// Every 64 bit number in the gateway's JSON is a string, because JSON numbers lose the bottom
	// of a lease id.
	std::optional<int64_t> read_number(const boost::json::object &object, const std::string &field)
	{
		if (!object.contains(field) || !object.at(field).is_string())
		{
			return std::nullopt;
		}

		int64_t number = 0;

		if (!boost::conversion::try_lexical_convert(std::string(object.at(field).as_string()), number))
		{
			return std::nullopt;
		}

		return number;
	}

	// Every answer from the gateway carries the revision the cluster was at when it answered, and
	// for a write that is the revision the write happened at.
	std::optional<int64_t> header_revision(const boost::json::object &response)
	{
		if (!response.contains("header") || !response.at("header").is_object())
		{
			return std::nullopt;
		}

		return read_number(response.at("header").as_object(), "revision");
	}

	// What a lost claim answers with: the key as it already stood, which names the node holding it
	// and the revision it took it at.
	std::optional<etcd::claim> read_claim(const boost::json::object &response)
	{
		if (!response.contains("responses") || !response.at("responses").is_array() ||
			response.at("responses").as_array().empty())
		{
			return std::nullopt;
		}

		const boost::json::value &answered = response.at("responses").as_array()[0];

		if (!answered.is_object() || !answered.as_object().contains("responseRange") ||
			!answered.as_object().at("responseRange").is_object())
		{
			return std::nullopt;
		}

		const boost::json::object &ranged = answered.as_object().at("responseRange").as_object();

		if (!ranged.contains("kvs") || !ranged.at("kvs").is_array() || ranged.at("kvs").as_array().empty() ||
			!ranged.at("kvs").as_array()[0].is_object())
		{
			return std::nullopt;
		}

		const boost::json::object &pair = ranged.at("kvs").as_array()[0].as_object();
		std::optional<int64_t> revision = read_number(pair, "create_revision");
		std::string holder;

		if (!revision || !pair.contains("value") || !pair.at("value").is_string() ||
			!base64::decode(std::string(pair.at("value").as_string()), &holder))
		{
			return std::nullopt;
		}

		etcd::claim claimed;

		claimed.holder = holder;
		claimed.revision = *revision;

		return claimed;
	}
}

etcd::client::client(const http::client &http, const std::vector<std::string> &etcd_endpoints):
	http_client(http),
	endpoints(etcd_endpoints)
{
}

std::optional<int64_t> etcd::client::grant_lease(int64_t ttl_seconds) const
{
	boost::json::object request { { "TTL", std::to_string(ttl_seconds) } };
	std::optional<boost::json::object> response = call("lease/grant", request, true);

	if (!response)
	{
		return std::nullopt;
	}

	return read_number(*response, "ID");
}

bool etcd::client::keep_alive(int64_t lease) const
{
	boost::json::object request { { "ID", std::to_string(lease) } };
	std::optional<boost::json::object> response = call("lease/keepalive", request, true);

	if (!response || !response->contains("result") || !response->at("result").is_object())
	{
		return false;
	}

	// A lease that has already expired is renewed to a time to live of nothing, rather than
	// refused, so the answer has to be read rather than counted.
	std::optional<int64_t> ttl = read_number(response->at("result").as_object(), "TTL");

	return ttl && *ttl > 0;
}

bool etcd::client::put(const std::string &key, const std::string &value, int64_t lease) const
{
	boost::json::object request {
		{ "key", base64::encode(key) },
		{ "value", base64::encode(value) },
		{ "lease", std::to_string(lease) }
	};

	return call("kv/put", request, true).has_value();
}

// One transaction rather than a read and then a write: "if nothing created this key, put it, and
// otherwise tell me what is there". Two nodes claiming the same partition at the same moment is
// therefore one winner and one that is told the winner's name, and never two leaders.
std::optional<etcd::claim> etcd::client::create(
	const std::string &key,
	const std::string &value,
	int64_t lease) const
{
	boost::json::object request {
		{ "compare", boost::json::array { boost::json::object {
			{ "key", base64::encode(key) },
			{ "target", "CREATE" },
			{ "result", "EQUAL" },
			{ "create_revision", "0" }
		} } },
		{ "success", boost::json::array { boost::json::object {
			{ "requestPut", boost::json::object {
				{ "key", base64::encode(key) },
				{ "value", base64::encode(value) },
				{ "lease", std::to_string(lease) }
			} }
		} } },
		{ "failure", boost::json::array { boost::json::object {
			{ "requestRange", boost::json::object { { "key", base64::encode(key) } } }
		} } }
	};

	std::optional<boost::json::object> response = call("kv/txn", request, true);

	if (!response)
	{
		return std::nullopt;
	}

	claim claimed;

	// The transaction succeeded, so this caller created the key. The revision it was created at is
	// the revision of the write itself, which the header carries.
	if (response->contains("succeeded") && response->at("succeeded").is_bool() &&
		response->at("succeeded").as_bool())
	{
		std::optional<int64_t> revision = header_revision(*response);

		if (!revision)
		{
			return std::nullopt;
		}

		claimed.held = true;
		claimed.holder = value;
		claimed.revision = *revision;

		return claimed;
	}

	// It was lost, and what came back instead is the key as it stands.
	return read_claim(*response);
}

std::map<std::string, std::string> etcd::client::range(const std::string &prefix) const
{
	boost::json::object request {
		{ "key", base64::encode(prefix) },
		{ "range_end", base64::encode(range_end(prefix)) }
	};

	std::optional<boost::json::object> response = call("kv/range", request, true);
	std::map<std::string, std::string> values;

	if (!response || !response->contains("kvs") || !response->at("kvs").is_array())
	{
		return values;
	}

	const boost::json::array &pairs = response->at("kvs").as_array();

	for (size_t i = 0; i < pairs.size(); i++)
	{
		if (!pairs[i].is_object())
		{
			continue;
		}

		const boost::json::object &pair = pairs[i].as_object();
		std::string key;
		std::string value;

		if (pair.contains("key") && pair.at("key").is_string() &&
			pair.contains("value") && pair.at("value").is_string() &&
			base64::decode(std::string(pair.at("key").as_string()), &key) &&
			base64::decode(std::string(pair.at("value").as_string()), &value))
		{
			values[key] = value;
		}
	}

	return values;
}

bool etcd::client::revoke(int64_t lease) const
{
	boost::json::object request { { "ID", std::to_string(lease) } };

	return call("lease/revoke", request, false).has_value();
}

const std::string &etcd::client::endpoint() const
{
	static const std::string none;

	return endpoints.empty() ? none : endpoints[current];
}

std::optional<boost::json::object> etcd::client::call(
	const std::string &method,
	const boost::json::object &body,
	bool every_member) const
{
	std::string document = boost::json::serialize(body);

	// Walking the members costs a timeout for each one that is not there, which is a wait a node
	// on its way out does not have: it is stopped for good after ten seconds.
	size_t attempts = every_member ? endpoints.size() : std::min<size_t>(endpoints.size(), 1);

	// Asking the members in turn, beginning with the one that answered last. A member that cannot
	// be reached, or that is up but not serving, is a reason to ask the next one; anything it
	// answers is the answer the whole cluster would give, and asking again would only be slower.
	//
	// The calls repeated this way are safe to repeat: writing the same key is idempotent, and a
	// lease granted twice because the first answer was lost expires on its own.
	for (size_t i = 0; i < attempts; i++)
	{
		size_t member = (current + i) % endpoints.size();

		http::request request {
			"POST",
			endpoints[member] + "/v3/" + method,
			document,
			{ "Content-Type: application/json" }
		};

		http::response response = http_client.send(request);

		if (!response.is_valid)
		{
			DEBUG("etcd " + method + " to " + endpoints[member] + " failed: " + response.message);

			continue;
		}

		if (response.status >= 500)
		{
			DEBUG("etcd " + method + " to " + endpoints[member] + " answered " + std::to_string(response.status) + ".");

			continue;
		}

		current = member;

		if (response.status != 200)
		{
			DEBUG("etcd " + method + " answered " + std::to_string(response.status) + ": " + response.body);

			return std::nullopt;
		}

		boost::system::error_code error;
		boost::json::value value = boost::json::parse(response.body, error);

		if (error || !value.is_object())
		{
			DEBUG("etcd " + method + " answered with something that is not a document: " + response.body);

			return std::nullopt;
		}

		return value.as_object();
	}

	DEBUG("No member of etcd answered " + method + ".");

	return std::nullopt;
}
