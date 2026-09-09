#ifndef REPOSITORY_SCRATCH_FILE_H
#define REPOSITORY_SCRATCH_FILE_H

#include <filesystem>
#include <string>

namespace repository
{
	// A file a transfer is built in and read out of, under a directory of the store's own. It goes
	// when it goes out of scope, whether what was writing it finished or threw: a store that leaked
	// one a transfer is a store that fills its own disk.
	class scratch_file
	{
		std::string file;

	public:
		scratch_file(const std::filesystem::path &directory, const std::string &name);

		~scratch_file();

		scratch_file(const scratch_file &) = delete;

		scratch_file &operator=(const scratch_file &) = delete;

		const std::string &path() const;

		// What is in the file, or nothing at all when there is no reading it.
		std::string read() const;

		// Whether the bytes were written. A caller that cannot write one cannot go on, and this is
		// the disk being full as often as it is anything else.
		bool write(const std::string &bytes);
	};
}

#endif
