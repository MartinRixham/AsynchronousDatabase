#ifndef REPOSITORY_REPOSITORY_H
#define REPOSITORY_REPOSITORY_H

#include <cstddef>
#include <optional>
#include <set>
#include <string>

#include "cluster/partition.h"
#include "record/record.h"
#include "scan/scan.h"
#include "table/table.h"
#include "storage_error.h"

namespace repository
{
	// How much of a table one file of a transfer walks. It is far larger than a page of a scan
	// because the point of a file is that a node moving a share of a terabyte pays one round trip
	// for it rather than one for every hundred records: past this the transfer is bound by the
	// bandwidth between two nodes rather than by the time to ask, and both of them hold a file this
	// size in memory while it travels.
	constexpr size_t max_file_bytes = 64 * 1024 * 1024;

	// Which of a table's records a file is to carry: the partitions the node asking for it holds,
	// and where in the table to resume, because a share larger than one file is several of them.
	//
	// The partitions are the asking node's own, so what a file carries is decided by the membership
	// that node read and never by the one this node holds. Two nodes a moment apart in what they
	// think the cluster is still agree on what was sent.
	struct share
	{
		cluster::partition_set partitions;

		std::string from;

		bool has_from = false;

		// The keys alone, for a caller deciding where records belong rather than moving them. A
		// walk that is not carrying values covers far more of a table for the same budget, which
		// is what makes clearing down a share one question instead of one for every key.
		bool values = true;

		// How much of the table one file walks. It is the caller's because the caller is what
		// knows how much it can hold: everything the server asks for asks for the whole budget,
		// and a test asks for a file it can write by hand.
		size_t bytes = max_file_bytes;
	};

	// What one file carried: the bytes, how many records went into them, and the key the walk
	// reached — which is where the file after this one resumes, and is a key the file itself may
	// not hold, because the budget is what was walked rather than what was taken.
	struct extract
	{
		std::string file;

		size_t records = 0;

		bool has_more = false;

		std::string last;
	};

	class repository
	{
	public:
		virtual void create_table(const table::table &table) = 0;

		virtual std::set<table::table> list_tables() const = 0;

		virtual bool has_table(const std::string &table_name) const = 0;

		virtual table::table read_table(const std::string &table_name) const = 0;

		virtual void delete_table(const std::string &table_name) = 0;

		virtual void write_record(const std::string &table_name, const record::record &record) = 0;

		virtual std::optional<std::string> read_record(const std::string &table_name, const std::string &key) const = 0;

		virtual void delete_record(const std::string &table_name, const std::string &key) = 0;

		virtual scan::page scan_records(const std::string &table_name, const scan::range &range) const = 0;

		// One file of the records of a table that belong to the partitions named. This is how a
		// store's records reach another node: a walk that pages a hundred records at a time over
		// HTTP is a round trip for every hundred, which no node holding a real share can finish.
		virtual extract export_records(const std::string &table_name, const share &wanted) const = 0;

		// Takes a file into a table and answers how many records it took. **A key this store
		// already holds is kept**: the file was written by a node that used to own the key, and
		// every write since the ownership moved came here, so what is here is the newer of the two.
		virtual size_t import_records(const std::string &table_name, const std::string &file) = 0;

		// Deletes the records of this table that the file also carries, and answers how many went.
		// The file is written by the node that owns those keys now, so a key in both stores is a
		// copy this one may give up — which is the whole of what makes clearing down safe. A key
		// the file carries and this store has nothing for is left alone rather than deleted, or a
		// pass would write a tombstone for every record it never held.
		virtual size_t clear_records(const std::string &table_name, const std::string &file) = 0;

		virtual void delete_records(const std::string &table_name, const scan::range &range) = 0;

		virtual bool is_write_stalled() const = 0;

		virtual std::string instance() const = 0;
	};
}

#endif
