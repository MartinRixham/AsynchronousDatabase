#ifndef REPOSITORY_ROCSKDB_REPOSITORY_H
#define REPOSITORY_ROCSKDB_REPOSITORY_H

#include <rocksdb/db.h>

#include <string>

#include "repository.h"

namespace repository
{
	class rocksdb_repository : public repository
	{
		rocksdb::DB *database;

		public:
			explicit rocksdb_repository(rocksdb::DB *db);

			void create_table(const std::string &name) override;
	};
}

#endif
