#ifndef REPOSITORY_REPOSITORY_H
#define REPOSITORY_REPOSITORY_H

#include <rocksdb/db.h>
#include <string>
#include <vector>

#include "table/table.h"

namespace repository
{
	class repository
	{
		public:
			virtual void create_table(const table::table &table) = 0;

			virtual std::vector<std::string> list_tables() = 0;

			virtual bool has_table(const std::string &name) = 0;
	};
}

#endif
