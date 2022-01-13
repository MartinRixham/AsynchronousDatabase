#ifndef REPOSITORY_ROCSKDB_REPOSITORY_H
#define REPOSITORY_ROCSKDB_REPOSITORY_H

#include <rocksdb/db.h>

#include <string>
#include <unordered_set>

#include "repository/repository.h"

namespace repository
{
	class fake_repository : public repository
	{
		std::unordered_set<std::string> tables;

		public:
			fake_repository();

			void create_table(const std::string &name) override;

			bool has_table(const std::string &name);
	};
}

#endif
