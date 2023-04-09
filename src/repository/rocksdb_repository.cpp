#include "rocksdb_repository.h"

repository::rocksdb_repository::rocksdb_repository(rocksdb::DB *db)
{
	database = db;
}

void repository::rocksdb_repository::create_table(const std::string &name)
{
	database->Put(rocksdb::WriteOptions(), "TABLE_" + name, "{ \"name\": \"" + name +"\" }");
}

std::vector<std::string> repository::rocksdb_repository::list_tables()
{
	rocksdb::ReadOptions options;
	rocksdb::Iterator *it = database->NewIterator(options);
	std::vector<std::string> tables;

	for(it->Seek("TABLE"); it->Valid(); it->Next())
	{
		tables.push_back(it->value().ToString());
	}

	return tables;
}
