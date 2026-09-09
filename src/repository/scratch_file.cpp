#include <fstream>
#include <sstream>

#include "scratch_file.h"

repository::scratch_file::scratch_file(const std::filesystem::path &directory, const std::string &name):
	file((directory / name).string())
{
}

repository::scratch_file::~scratch_file()
{
	std::error_code ignored;

	// A file an ingest moved into the store is one that is already gone, and a file that was never
	// written is one that was never there, so neither is an error worth carrying out of a
	// destructor.
	std::filesystem::remove(file, ignored);
}

const std::string &repository::scratch_file::path() const
{
	return file;
}

std::string repository::scratch_file::read() const
{
	std::ifstream reading(file, std::ios::binary);
	std::ostringstream bytes;

	bytes << reading.rdbuf();

	return bytes.str();
}

bool repository::scratch_file::write(const std::string &bytes)
{
	std::ofstream writing(file, std::ios::binary | std::ios::trunc);

	writing.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	writing.close();

	return writing.good();
}
