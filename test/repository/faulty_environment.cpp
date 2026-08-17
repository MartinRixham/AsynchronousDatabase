#include "faulty_environment.h"

rocksdb::Status repository::faulty_environment::ReopenWritableFile(
	const std::string &f, std::unique_ptr<rocksdb::WritableFile> *r, const rocksdb::EnvOptions &o)
{
	return rocksdb::Status::IOError("simulated failure");
}
