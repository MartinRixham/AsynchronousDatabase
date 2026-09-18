#include <algorithm>

#include <boost/json.hpp>
#include <boost/lexical_cast/try_lexical_convert.hpp>

#include "base64/base64.h"
#include "log.h"
#include "etcd_client.h"

namespace
{
	// A prefix of nothing but 0xff bytes has no end, and "\0" is how etcd spells "to the end of
	// the keyspace".
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

	std::optional<int64_t> header_revision(const boost::json::object &response)
	{
		if (!response.contains("header") || !response.at("header").is_object())
		{
			return std::nullopt;
		}

		return read_number(response.at("header").as_object(), "revision");
	}

	// Whether a watch goes on after one of its answers, which is every answer but etcd ending it. A
	// line that is not a document says nothing either way.
	bool still_watching(const std::string &line)
	{
		boost::system::error_code error;
		boost::json::value value = boost::json::parse(line, error);

		if (error || !value.is_object())
		{
			return true;
		}

		const boost::json::object &answer = value.as_object();

		if (answer.contains("error"))
		{
			return false;
		}

		return !answer.contains("result") ||
			   !answer.at("result").is_object() ||
			   !answer.at("result").as_object().contains("canceled") ||
			   !answer.at("result").as_object().at("canceled").is_bool() ||
			   !answer.at("result").as_object().at("canceled").as_bool();
	}

	std::optional<etcd::claim> read_claim(const boost::json::object &response)
	{
		if (!response.contains("responses") ||
			!response.at("responses").is_array() ||
			response.at("responses").as_array().empty())
		{
			return std::nullopt;
		}

		const boost::json::value &answered = response.at("responses").as_array()[0];

		// The gateway reads a request written either way and writes an answer only one way, so the
		// field a transaction is asked in is "requestRange" and the field it answers in is not.

		if (!answered.is_object() ||
			!answered.as_object().contains("response_range") ||
			!answered.as_object().at("response_range").is_object())
		{
			return std::nullopt;
		}

		const boost::json::object &ranged = answered.as_object().at("response_range").as_object();

		if (!ranged.contains("kvs") ||
			!ranged.at("kvs").is_array() ||
			ranged.at("kvs").as_array().empty() ||
			!ranged.at("kvs").as_array()[0].is_object())
		{
			return std::nullopt;
		}

		const boost::json::object &pair = ranged.at("kvs").as_array()[0].as_object();
		std::optional<int64_t> revision = read_number(pair, "create_revision");

		if (!revision || !pair.contains("value") || !pair.at("value").is_string())
		{
			return std::nullopt;
		}

		std::optional<std::string> holder = base64::decode(std::string(pair.at("value").as_string()));

		if (!holder)
		{
			return std::nullopt;
		}

		etcd::claim claimed;

		claimed.holder = *holder;
		claimed.revision = *revision;

		return claimed;
	}
}

etcd::client::client(const http::client &http, const std::vector<std::string> &etcd_endpoints, long timeout):
	http_client(http),
	endpoints(etcd_endpoints),
	timeout_seconds(timeout)
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

	std::optional<int64_t> ttl = read_number(response->at("result").as_object(), "TTL");

	return ttl && *ttl > 0;
}

bool etcd::client::put(const std::string &key, const std::string &value, int64_t lease) const
{
	boost::json::object request { { "key", base64::encode(key) },
								  { "value", base64::encode(value) },
								  { "lease", std::to_string(lease) } };

	return call("kv/put", request, true).has_value();
}

std::optional<etcd::claim> etcd::client::create(const std::string &key, const std::string &value, int64_t lease) const
{
	boost::json::object request {
		{ "compare",
		  boost::json::array { boost::json::object { { "key", base64::encode(key) },
													 { "target", "CREATE" },
													 { "result", "EQUAL" },
													 { "create_revision", "0" } } } },
		{ "success",
		  boost::json::array {
			  boost::json::object { { "requestPut",
									  boost::json::object { { "key", base64::encode(key) },
															{ "value", base64::encode(value) },
															{ "lease", std::to_string(lease) } } } } } },
		{ "failure",
		  boost::json::array {
			  boost::json::object { { "requestRange", boost::json::object { { "key", base64::encode(key) } } } } } }
	};

	std::optional<boost::json::object> response = call("kv/txn", request, true);

	if (!response)
	{
		return std::nullopt;
	}

	claim claimed;

	if (response->contains("succeeded") && response->at("succeeded").is_bool() && response->at("succeeded").as_bool())
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

	return read_claim(*response);
}

bool etcd::client::remove(const std::string &key, const std::string &value) const
{
	boost::json::object request {
		{ "compare",
		  boost::json::array { boost::json::object { { "key", base64::encode(key) },
													 { "target", "VALUE" },
													 { "result", "EQUAL" },
													 { "value", base64::encode(value) } } } },
		{ "success",
		  boost::json::array { boost::json::object {
			  { "requestDeleteRange", boost::json::object { { "key", base64::encode(key) } } } } } }
	};

	std::optional<boost::json::object> response = call("kv/txn", request, true);

	return response &&
		   response->contains("succeeded") &&
		   response->at("succeeded").is_bool() &&
		   response->at("succeeded").as_bool();
}

std::optional<std::map<std::string, std::string>> etcd::client::range(const std::string &prefix) const
{
	boost::json::object request { { "key", base64::encode(prefix) },
								  { "range_end", base64::encode(range_end(prefix)) } };

	return read(request);
}

std::optional<std::map<std::string, std::string>> etcd::client::get(const std::string &key) const
{
	return read(boost::json::object { { "key", base64::encode(key) } });
}

std::optional<std::map<std::string, std::string>> etcd::client::read(const boost::json::object &request) const
{
	std::optional<boost::json::object> response = call("kv/range", request, true);

	if (!response)
	{
		return std::nullopt;
	}

	std::map<std::string, std::string> values;

	// The gateway leaves the field out of a range that found nothing.
	if (!response->contains("kvs"))
	{
		return values;
	}

	if (!response->at("kvs").is_array())
	{
		return std::nullopt;
	}

	const boost::json::array &pairs = response->at("kvs").as_array();

	for (const auto &held : pairs)
	{
		if (!held.is_object())
		{
			continue;
		}

		const boost::json::object &pair = held.as_object();

		if (!pair.contains("key") ||
			!pair.at("key").is_string() ||
			!pair.contains("value") ||
			!pair.at("value").is_string())
		{
			continue;
		}

		std::optional<std::string> key = base64::decode(std::string(pair.at("key").as_string()));
		std::optional<std::string> value = base64::decode(std::string(pair.at("value").as_string()));

		if (key && value)
		{
			values[*key] = *value;
		}
	}

	return values;
}

bool etcd::client::revoke(int64_t lease) const
{
	boost::json::object request { { "ID", std::to_string(lease) } };

	return call("lease/revoke", request, false).has_value();
}

void etcd::client::watch(const std::string &prefix, const std::function<void()> &changed, const std::stop_token &stop)
	const
{
	if (endpoints.empty())
	{
		return;
	}

	boost::json::object watched { { "key", base64::encode(prefix) },
								  { "range_end", base64::encode(range_end(prefix)) } };

	http::request request { "POST",
							endpoint() + "/v3/watch",
							boost::json::serialize(boost::json::object { { "create_request", watched } }),
							{ "Content-Type: application/json" } };

	// The gateway writes one document to a line, and a piece of the stream ends wherever the
	// network cut it.
	std::string partial;

	std::expected<http::response, std::string> response = http_client.stream(
		request,
		[&partial, &changed](std::string_view piece)
		{
			partial.append(piece);

			size_t end = 0;

			while ((end = partial.find('\n')) != std::string::npos)
			{
				std::string line = partial.substr(0, end);

				partial.erase(0, end + 1);

				if (line.find_first_not_of(" \t\r") == std::string::npos)
				{
					continue;
				}

				if (!still_watching(line))
				{
					DEBUG("etcd ended a watch: " + line);

					return false;
				}

				changed();
			}

			return true;
		},
		stop);

	if (!response)
	{
		DEBUG("etcd watch at " + request.url + " ended: " + response.error());
	}
}

const std::string &etcd::client::endpoint() const
{
	static const std::string none;

	return endpoints.empty() ? none : endpoints[current.load()];
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

	// The calls repeated this way are safe to repeat: writing the same key is idempotent, and a
	// lease granted twice expires on its own.
	for (size_t i = 0; i < attempts; i++)
	{
		size_t member = (current.load() + i) % endpoints.size();

		http::request request { "POST",
								endpoints[member] + "/v3/" + method,
								document,
								{ "Content-Type: application/json" } };

		std::expected<http::response, std::string> expected_response = http_client.send(request, timeout_seconds);

		if (!expected_response)
		{
			DEBUG("etcd " + method + " to " + endpoints[member] + " failed: " + expected_response.error());

			continue;
		}

		http::response response = *expected_response;

		if (response.status >= 500)
		{
			DEBUG("etcd " + method + " to " + endpoints[member] + " answered " + std::to_string(response.status) + ".");

			continue;
		}

		current.store(member);

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
