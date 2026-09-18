#pragma once

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "repository/scratch_file.h"

// What is in a file a store exported. The server never reads one back itself, sending it from where
// it was written, so a test that hands one to another store reads it here. A walk that took no
// records has no file, which is no bytes.
inline std::string bytes_of(const std::shared_ptr<const repository::scratch_file> &file)
{
	if (!file)
	{
		return "";
	}

	std::ifstream reading(file->path(), std::ios::binary);
	std::ostringstream bytes;

	bytes << reading.rdbuf();

	return bytes.str();
}
