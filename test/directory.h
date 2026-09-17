#pragma once

#include <filesystem>
#include <string>

// A test's store lives under the temporary directory rather than at a fixed path, because the test
// binary runs its shards as processes of their own, each given a TMPDIR of its own, and RocksDB
// locks the directory it opens.
inline std::string test_directory(const std::string &name)
{
	return (std::filesystem::temp_directory_path() / name).string();
}
