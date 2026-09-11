#ifndef TABLE_SCHEMA_H
#define TABLE_SCHEMA_H

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "record/record.h"
#include "table.h"

namespace table
{
	// One name's place in the schema: the table as it stands, or the delete that took it away. A
	// name has an entry either way, and the tombstone is what lets an absence be told from a
	// create that never arrived — which a node holding a table no peer names cannot otherwise do.
	struct entry
	{
		bool live = false;

		// What orders two nodes' answers for this name. It is **per name and never per schema**:
		// one version over the whole document would vouch for operations the node never applied,
		// a node that missed one and took the next carrying a version as high as a node that took
		// both. That is a replication log, and there is none here — records are ordered per key
		// for the same reason.
		record::version stamp;

		// The table document, as the API returns it. A tombstone carries none.
		boost::json::object json;
	};

	// Every name the store has heard of, live or dropped, and the version each stands at. It is
	// held and moved as **one record**, so a node asks for the schema once and takes it whole.
	class schema
	{
		std::map<std::string, entry> entries;

	public:
		std::set<table> tables() const;

		std::set<std::string> names() const;

		bool has(const std::string &name) const;

		table read(const std::string &name) const;

		// Nothing at all for a name this schema has never held, which is what tells a name it
		// never heard of from one it dropped.
		std::optional<entry> read_entry(const std::string &name) const;

		void create(const table &table, const record::version &stamp);

		void remove(const std::string &name, const record::version &stamp);

		// What a merge did to one name, because a table that went takes a column family with it
		// and one that arrived needs one made.
		struct change
		{
			std::string name;

			bool live = false;
		};

		// Takes every entry of `named` that stands later than this schema's own for that name.
		// **A live entry never drops a column family**: two live entries are one table whose
		// stamp was issued twice — a create carried to a node that had missed it, or a leader
		// that had — and dropping on that would take the records with it. Only a tombstone drops.
		std::vector<change> merge(const schema &named);

		boost::json::object json() const;
	};

	schema to_schema(const std::string &json);
}

#endif
