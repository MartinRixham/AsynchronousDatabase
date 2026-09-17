#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <curl/curl.h>

extern char **environ;

namespace
{
	struct shard
	{
		int index = 0;
		pid_t pid = 0;
		std::filesystem::path output;
	};

	int shard_count()
	{
		const char *configured = std::getenv("GTEST_TOTAL_SHARDS");

		if (configured != nullptr)
		{
			return std::atoi(configured);
		}

		return static_cast<int>(std::thread::hardware_concurrency());
	}

	// The suite spends its time waiting on timeouts and other threads rather than computing, so it is
	// run as a process to a shard rather than a thread: a store, the environment and libcurl's globals
	// are all a process's own, and gtest shards a suite across processes and not threads.
	bool runs_in_process(int shards)
	{
		return std::getenv("GTEST_SHARD_INDEX") != nullptr || shards <= 1 || GTEST_FLAG_GET(list_tests) ||
			GTEST_FLAG_GET(filter) != "*";
	}

	std::optional<shard> spawn(const std::vector<char *> &arguments, int index, int shards)
	{
		std::filesystem::path directory = std::filesystem::temp_directory_path() / "asyncdb-shards" /
			std::to_string(index);
		std::filesystem::remove_all(directory);
		std::filesystem::create_directories(directory);

		std::filesystem::path output = directory / "output";
		std::vector<std::string> variables = {
			"GTEST_TOTAL_SHARDS=" + std::to_string(shards),
			"GTEST_SHARD_INDEX=" + std::to_string(index),
			"TMPDIR=" + directory.string(),
		};
		std::vector<char *> environment;
		char **end = environ;

		while (*end != nullptr)
		{
			++end;
		}

		std::ranges::copy_if(environ, end, std::back_inserter(environment), [](const char *variable) {
			std::string name(variable, std::string(variable).find('='));

			return name != "GTEST_TOTAL_SHARDS" && name != "GTEST_SHARD_INDEX" && name != "TMPDIR";
		});
		std::ranges::transform(variables, std::back_inserter(environment), [](auto &variable) { return variable.data(); });
		environment.push_back(nullptr);

		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
		posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

		pid_t pid = 0;
		int error = posix_spawn(&pid, arguments[0], &actions, nullptr, arguments.data(), environment.data());
		posix_spawn_file_actions_destroy(&actions);

		if (error != 0)
		{
			std::cout << "[  FAILED  ] Shard " << index << " did not start: " << std::strerror(error) << std::endl;

			return std::nullopt;
		}

		return shard { index, pid, output };
	}

	// A shard's output is held until it has finished and then printed whole, so the suites of two
	// shards are never interleaved line by line.
	int run_shards(const std::vector<char *> &arguments, int shards)
	{
		std::vector<shard> running;
		int failed = 0;

		for (int index = 0; index < shards; ++index)
		{
			std::optional<shard> started = spawn(arguments, index, shards);

			if (started)
			{
				running.push_back(*started);
			}
			else
			{
				++failed;
			}
		}

		for (size_t remaining = running.size(); remaining > 0; --remaining)
		{
			int status;
			pid_t pid = wait(&status);

			auto finished = std::ranges::find(running, pid, &shard::pid);
			int index = finished->index;

			std::cout << "[----------] Shard " << index << " of " << shards << std::endl;
			std::cout << std::ifstream(finished->output).rdbuf() << std::flush;

			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			{
				std::cout << "[  FAILED  ] Shard " << index << std::endl;
				++failed;
			}
		}

		std::cout << "[==========] " << shards - failed << " of " << shards << " shards passed." << std::endl;

		return failed == 0 ? 0 : 1;
	}
}

int main(int argc, char *argv[])
{
	// InitGoogleTest takes its own flags out of argv, and a shard is given them back.
	std::vector<char *> arguments(argv, argv + argc);
	arguments.push_back(nullptr);

	testing::InitGoogleTest(&argc, argv);

	int shards = shard_count();

	if (!runs_in_process(shards))
	{
		return run_shards(arguments, shards);
	}

	// gtest refuses a total with no index, and a total of one is how a run is kept in one process.
	if (std::getenv("GTEST_TOTAL_SHARDS") != nullptr && std::getenv("GTEST_SHARD_INDEX") == nullptr)
	{
		setenv("GTEST_SHARD_INDEX", "0", 1);
	}

	// The tests drive servers over libcurl and a cluster talks to etcd over it, so libcurl is
	// initialised once here rather than by whichever handle happens to be created first.
	curl_global_init(CURL_GLOBAL_DEFAULT);

	// Nothing calls curl_global_cleanup: a handle is held for the life of the thread that made
	// it, and the one belonging to this thread is closed after main has returned, which is after
	// anything here could have cleaned up under it. What libcurl holds is reachable, not lost.
	return RUN_ALL_TESTS();
}
