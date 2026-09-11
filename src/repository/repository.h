#ifndef REPOSITORY_REPOSITORY_H
#define REPOSITORY_REPOSITORY_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

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

		// **`from` is exclusive and `to` is inclusive**, which is not the way a scan's range reads
		// and is what makes the two chain: `from` is where the last file of this walk got to, and
		// a share cut into pieces is (nothing, first], (first, second], (second, nothing].
		std::string from;

		bool has_from = false;

		std::string to;

		bool has_to = false;

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

		virtual scan::page scan_records(const std::string &table_name, const scan::range &range) const = 0;

		// One file of the records of a table that belong to the partitions named. This is how a
		// store's records reach another node: a walk that pages a hundred records at a time over
		// HTTP is a round trip for every hundred, which no node holding a real share can finish.
		virtual extract export_records(const std::string &table_name, const share &wanted) const = 0;

		// Keys that cut a table into roughly equal pieces by size, for a walk that several workers
		// share: one fewer than the number of ways asked for, in order. Fewer than that is a table
		// there is not enough of to cut up, and none at all is one worker's work.
		//
		// It is approximate on purpose. What it is for is keeping workers busy, and a piece that is
		// half again the size of another costs a little of that and nothing else.
		virtual std::vector<std::string> split_points(const std::string &table_name, size_t ways) const = 0;

		// Takes a file into a table and answers how many records it took. **A key this store holds
		// at a later version is kept**: the record here was written after the one the file carries,
		// whoever owns the key now. An earlier one is replaced, which is what catches up a copy
		// that was not there for a write the others took.
		virtual size_t import_records(const std::string &table_name, const std::string &file) = 0;

		// Deletes the records of this table that the file also carries at a version at least as
		// late, and answers how many went. The file is written by the node that owns those keys
		// now, so a key in both stores is a copy this one may give up — unless what is here was
		// written later, which is a copy the owner has yet to catch up on and not one to drop. A
		// key the file carries and this store has nothing for is left alone rather than deleted,
		// or a pass would write a tombstone for every record it never held.
		virtual size_t clear_records(const std::string &table_name, const std::string &file) = 0;

		// The count for the next write this node orders, which rises for as long as the store
		// lives. It is handed out in blocks reserved on disk rather than one at a time, so a write
		// costs nothing to stamp and a process that restarts carries on above every count the one
		// before it issued — which is what a leader keeping its claim across a restart needs.
		virtual uint64_t next_count() = 0;

		virtual bool is_write_stalled() const = 0;

		virtual std::string instance() const = 0;
	};
}

#endif
