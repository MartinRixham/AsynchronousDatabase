#ifndef REPOSITORY_ROCSKDB_REPOSITORY_H
#define REPOSITORY_ROCSKDB_REPOSITORY_H

#include <rocksdb/db.h>
#include <string>
#include <vector>

#include "repository.h"

namespace repository
{
	class rocksdb_repository : public repository
	{
		rocksdb::DB *database;

		public:
			explicit rocksdb_repository();

			void create_table(const std::string &name) override;

			virtual std::vector<std::string> list_tables() override;

			~rocksdb_repository();
	};
}

#endif
