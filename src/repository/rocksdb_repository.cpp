#include "rocksdb_repository.h"

repository::rocksdb_repository::rocksdb_repository(rocksdb::DB *db)
{
	database = db;
}

void repository::rocksdb_repository::create_table(const std::string &name)
{
	database->Put(rocksdb::WriteOptions(), "TABLE_" + name, "{ \"name\": \"" + name +"\" }");
}
