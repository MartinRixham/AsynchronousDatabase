#ifndef REPOSITORY_ROCSKDB_REPOSITORY_H
#define REPOSITORY_ROCSKDB_REPOSITORY_H

#include "repository/repository.h"

namespace repository
{
	class fake_repository : public repository
	{
		std::map<std::string, std::string> tables;

		public:
			fake_repository();

			void create_table(const table::table &table) override;

			std::set<table::table> list_tables() override;

			bool has_table(const table::table &table) override;
	};
}

#endif
