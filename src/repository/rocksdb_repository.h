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

			bool has_table(const std::string &table_name) const override;

			table::table read_table(const std::string &table_name) const override;

			~rocksdb_repository();
	};
}

#endif
