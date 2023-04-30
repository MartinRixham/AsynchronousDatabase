#ifndef REPOSITORY_ROCSKDB_REPOSITORY_H
#define REPOSITORY_ROCSKDB_REPOSITORY_H

#include <rocksdb/db.h>

#include "repository.h"

namespace repository
{
	class rocksdb_repository : public repository
	{
		rocksdb::DB *database;

		public:
			explicit rocksdb_repository();

			void create_table(const table::table &table) override;

			std::set<table::table> list_tables() override;

			bool has_table(const table::table &table) override;

			~rocksdb_repository();
	};
}

#endif
