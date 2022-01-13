#include "repository.h"

repository::repository::repository(rocksdb::DB *db)
{
    database = db;
}

void repository::repository::create_table(const std::string &name)
{
    database->Put(rocksdb::WriteOptions(), "TABLE_" + name, "{ \"name\": \"" + name +"\" }");
}
