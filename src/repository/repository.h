#ifndef REPOSITORY_REPOSITORY_H
#define REPOSITORY_REPOSITORY_H

#include <rocksdb/db.h>
#include <set>

#include "table/table.h"

namespace repository
{
	class repository
	{
		public:
			virtual void create_table(const table::table &table) = 0;

			virtual std::set<table::table> list_tables() = 0;

			virtual bool has_table(const table::table &table) = 0;
	};
}

#endif
