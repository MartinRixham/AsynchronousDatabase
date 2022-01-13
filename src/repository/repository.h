#ifndef REPOSITORY_REPOSITORY_H
#define REPOSITORY_REPOSITORY_H

#include <rocksdb/db.h>

#include <string>

namespace repository
{
    class repository
    {
        rocksdb::DB *database;

        public:
            explicit repository(rocksdb::DB *db);

            void create_table(const std::string &name);
    };
}

#endif
