#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "log.h"
#include "cluster/placements.h"
#include "record/record.h"
#include "table/table.h"
#include "url/url.h"
#include "rebuild.h"

namespace
{
	router::request scan_request(const std::string &table, const std::string &from, bool has_from, size_t page)
	{
		router::request request;

		request.method = boost::beast::http::verb::get;
		request.path = std::vector<std::string> { "table", table, "key" };
		request.query = "limit=" + std::to_string(page) + "&values=true";

		if (has_from)
		{
			request.query += "&from=" + url::encode(from);
		}

		return request;
	}

	bool out_of_time(const std::chrono::steady_clock::time_point &deadline)
	{
		return std::chrono::steady_clock::now() >= deadline;
	}

	router::request table_request()
	{
		router::request request;

		request.method = boost::beast::http::verb::get;
		request.path = std::vector<std::string> { "table" };

		return request;
	}

	std::string field(const boost::json::object &object, const std::string &name)
	{
		if (!object.contains(name) || !object.at(name).is_string())
		{
			return "";
		}

		return std::string(object.at(name).as_string());
	}

	// What a rebuild took from one node or from one zone: whether the whole of it was read, and how
	// many of its records this node now holds.
	struct restored
	{
		bool whole = false;

		size_t records = 0;
	};

	std::optional<std::vector<table::table>> read_tables(
		const cluster::cluster &nodes,
		const std::vector<std::string> &zone,
		const std::chrono::steady_clock::time_point &deadline)
	{
		for (size_t i = 0; i < zone.size(); i++)
		{
			if (out_of_time(deadline))
			{
				return std::nullopt;
			}

			router::response answer = nodes.send(zone[i], table_request());

			if (answer.status != boost::beast::http::status::ok ||
				!answer.json.contains("tables") || !answer.json.at("tables").is_array())
			{
				DEBUG("Node " + zone[i] + " did not name its tables for a rebuild.");

				continue;
			}

			const boost::json::array &listed = answer.json.at("tables").as_array();
			std::vector<table::table> tables;

			for (size_t j = 0; j < listed.size(); j++)
			{
				if (!listed[j].is_object() || field(listed[j].as_object(), "name").empty())
				{
					continue;
				}

				tables.push_back(table::to_table(boost::json::serialize(listed[j])));
			}

			return tables;
		}

		return std::nullopt;
	}

	restored copy_table(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::string &node,
		const std::string &name,
		size_t page,
		const std::chrono::steady_clock::time_point &deadline)
	{
		restored taken;
		std::string from;
		bool has_from = false;

		// Where a key belongs is decided by its partition, so one walk asks that 256 times rather
		// than once for every key of the zone being read.
		cluster::placements where(nodes);

		while (true)
		{
	// The page boundary is the only place a rebuild can be given up: what is written already
	// is this node's own, and the page not asked for is all that is lost.
			if (out_of_time(deadline))
			{
				DEBUG("A scan of \"" + name + "\" on " + node + " ran out of time for a rebuild.");

				return taken;
			}

			router::response answer = nodes.send(node, scan_request(name, from, has_from, page));

			if (answer.status != boost::beast::http::status::ok ||
				!answer.json.contains("records") || !answer.json.at("records").is_array())
			{
				DEBUG("Node " + node + " did not answer a scan of \"" + name + "\" for a rebuild.");

				return taken;
			}

			const boost::json::array &records = answer.json.at("records").as_array();
			std::string last;
			bool has_last = false;
			bool read_any = false;

			for (size_t i = 0; i < records.size(); i++)
			{
				if (!records[i].is_object() || !records[i].as_object().contains("key") ||
					!records[i].as_object().at("key").is_string())
				{
					continue;
				}

				const boost::json::object &object = records[i].as_object();
				std::string key = field(object, "key");

				last = key;
				has_last = true;

				if (has_from && key == from)
				{
					continue;
				}

				read_any = true;

				if (where.of(key).local)
				{
					repository.write_record(name, record::valid_record(key, field(object, "value")));

					taken.records++;
				}
			}

			if (!answer.json.contains("next"))
			{
				taken.whole = true;

				return taken;
			}

			if (!has_last || !read_any)
			{
				DEBUG("A scan of \"" + name + "\" on " + node + " made no progress, so the rebuild stops.");

				return taken;
			}

			from = last;
			has_from = true;
		}
	}

	restored from_zone(
		repository::repository &repository,
		const cluster::cluster &nodes,
		const std::vector<std::string> &zone,
		size_t page,
		const std::chrono::steady_clock::time_point &deadline)
	{
		restored taken;
		std::optional<std::vector<table::table>> tables = read_tables(nodes, zone, deadline);

		if (!tables)
		{
			return taken;
		}

		for (size_t i = 0; i < tables->size(); i++)
		{
			repository.create_table((*tables)[i]);
		}

		for (size_t i = 0; i < tables->size(); i++)
		{
			const std::string &name = (*tables)[i].name;

			for (size_t j = 0; j < zone.size(); j++)
			{
				restored copied = copy_table(repository, nodes, zone[j], name, page, deadline);

				taken.records += copied.records;

				if (!copied.whole)
				{
					return taken;
				}
			}
		}

		taken.whole = true;

		return taken;
	}
}

size_t rebuild::rebuild(
	repository::repository &repository,
	const cluster::cluster &nodes,
	size_t page,
	long seconds)
{
	if (!repository.list_tables().empty())
	{
		return 0;
	}

	std::vector<std::vector<std::string>> zones = nodes.zones();

	if (zones.size() < 2)
	{
		return 0;
	}

	std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

	for (size_t i = 1; i < zones.size(); i++)
	{
		if (zones[i].empty())
		{
			continue;
		}

		if (out_of_time(deadline))
		{
			DEBUG("A rebuild ran out of time, so this node starts with what it has.");

			return 0;
		}

		restored taken = from_zone(repository, nodes, zones[i], page, deadline);

		if (taken.whole)
		{
			DEBUG("Rebuilt " + std::to_string(taken.records) + " records before joining.");

			return taken.records;
		}

		DEBUG("A node of a zone did not answer, so the rebuild asks the next zone.");
	}

	DEBUG("No zone answered a rebuild, so this node starts with what it has.");

	return 0;
}
