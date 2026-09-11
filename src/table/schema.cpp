#include "schema.h"

namespace
{
	bool is_newer(const record::version &stamp, const record::version &than)
	{
		return stamp.term != than.term ? stamp.term > than.term : stamp.count > than.count;
	}

	uint64_t number(const boost::json::object &json, const std::string &field)
	{
		if (!json.contains(field))
		{
			return 0;
		}

		const boost::json::value &held = json.at(field);

		if (held.is_uint64())
		{
			return held.as_uint64();
		}

		return held.is_int64() && held.as_int64() > 0 ? static_cast<uint64_t>(held.as_int64()) : 0;
	}
}

std::set<table::table> table::schema::tables() const
{
	std::set<table> live;

	for (std::map<std::string, entry>::const_iterator it = entries.begin(); it != entries.end(); ++it)
	{
		if (it->second.live)
		{
			live.insert(table { true, it->first, it->second.json, "", "" });
		}
	}

	return live;
}

std::set<std::string> table::schema::names() const
{
	std::set<std::string> live;

	for (std::map<std::string, entry>::const_iterator it = entries.begin(); it != entries.end(); ++it)
	{
		if (it->second.live)
		{
			live.insert(it->first);
		}
	}

	return live;
}

bool table::schema::has(const std::string &name) const
{
	std::map<std::string, entry>::const_iterator held = entries.find(name);

	return held != entries.end() && held->second.live;
}

table::table table::schema::read(const std::string &name) const
{
	std::map<std::string, entry>::const_iterator held = entries.find(name);

	if (held == entries.end() || !held->second.live)
	{
		return invalid_table("table_not_found", "No table named \"" + name + "\".");
	}

	return table { true, name, held->second.json, "", "" };
}

std::optional<table::entry> table::schema::read_entry(const std::string &name) const
{
	std::map<std::string, entry>::const_iterator held = entries.find(name);

	return held == entries.end() ? std::nullopt : std::optional<entry>(held->second);
}

void table::schema::create(const table &table, const record::version &stamp)
{
	entries[table.name] = entry { true, stamp, table.json };
}

void table::schema::remove(const std::string &name, const record::version &stamp)
{
	entries[name] = entry { false, stamp, boost::json::object() };
}

std::vector<table::schema::change> table::schema::merge(const schema &named)
{
	std::vector<change> changed;

	for (std::map<std::string, entry>::const_iterator it = named.entries.begin(); it != named.entries.end(); ++it)
	{
		std::map<std::string, entry>::const_iterator held = entries.find(it->first);
		bool had = held != entries.end() && held->second.live;

		if (held != entries.end() && !is_newer(it->second.stamp, held->second.stamp))
		{
			continue;
		}

		entries[it->first] = it->second;

		// A live entry that replaced a live one is the same table twice over — a create carried
		// to a node that had missed it is stamped again — so the column family stays where it is
		// and only the document moved. It is a name going the other way that has one to make or
		// to drop.
		if (had != it->second.live)
		{
			changed.push_back(change { it->first, it->second.live });
		}
	}

	return changed;
}

boost::json::object table::schema::json() const
{
	boost::json::array named;

	for (std::map<std::string, entry>::const_iterator it = entries.begin(); it != entries.end(); ++it)
	{
		boost::json::object one {
			{ "name", boost::json::string(it->first) },
			{ "live", it->second.live },
			{ "term", static_cast<int64_t>(it->second.stamp.term) },
			{ "count", static_cast<int64_t>(it->second.stamp.count) }
		};

		if (it->second.live)
		{
			one["table"] = it->second.json;
		}

		named.push_back(one);
	}

	return boost::json::object { { "schema", named } };
}

table::schema table::to_schema(const std::string &json)
{
	schema read;
	boost::system::error_code error;
	boost::json::value document = boost::json::parse(json, error);

	if (error || !document.is_object())
	{
		return read;
	}

	const boost::json::object &object = document.as_object();

	if (!object.contains("schema") || !object.at("schema").is_array())
	{
		return read;
	}

	const boost::json::array &named = object.at("schema").as_array();

	for (size_t i = 0; i < named.size(); i++)
	{
		if (!named[i].is_object())
		{
			continue;
		}

		const boost::json::object &one = named[i].as_object();

		if (!one.contains("name") || !one.at("name").is_string())
		{
			continue;
		}

		std::string name = std::string(one.at("name").as_string());
		record::version stamp { number(one, "term"), number(one, "count") };

		if (one.contains("live") && one.at("live").is_bool() && one.at("live").as_bool()
			&& one.contains("table") && one.at("table").is_object())
		{
			read.create(table { true, name, one.at("table").as_object(), "", "" }, stamp);
		}
		else
		{
			read.remove(name, stamp);
		}
	}

	return read;
}
