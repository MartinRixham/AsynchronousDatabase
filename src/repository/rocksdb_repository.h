#ifndef REPOSITORY_ROCSKDB_REPOSITORY_H
#define REPOSITORY_ROCSKDB_REPOSITORY_H

#include "repository.h"

namespace repository
{
	class rocksdb_repository : public repository
	{
		rocksdb::DB *database;

		public:
			rocksdb_repository();

			void create_table(const table::table &table) override;

			std::set<table::table> list_tables() const override;

			bool has_table(const table::table &table) const override;

			~rocksdb_repository();
	};
}

#endif
