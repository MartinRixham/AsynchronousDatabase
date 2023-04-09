#ifndef REPOSITORY_REPOSITORY_H
#define REPOSITORY_REPOSITORY_H

#include <rocksdb/db.h>

#include <string>
#include <vector>

namespace repository
{
	class repository
	{
		public:
			virtual void create_table(const std::string &name) = 0;

			virtual std::vector<std::string> list_tables() = 0;
	};
}

#endif
