#include <rocksdb/env.h>
#include <rocksdb/db.h>

namespace repository
{
  class faulty_environment : public rocksdb::EnvWrapper
  {
    rocksdb::Status ReopenWritableFile(
      const std::string& f,
      std::unique_ptr<rocksdb::WritableFile>* r,
      const rocksdb::EnvOptions& o) override; 
  };
}
