#ifndef TRANSFER_RELAY_H
#define TRANSFER_RELAY_H

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

namespace transfer
{
	// The handoff between the thread of a worker that asks for files and the thread that takes them
	// into the store. **One slot**, so the asking runs exactly one file ahead: the next file is on
	// its way while the last one is being read, and the memory a walk holds is two files a worker
	// rather than however many the network can outrun the store by.
	class relay
	{
		std::mutex mutex;

		std::condition_variable filled;

		std::condition_variable emptied;

		std::optional<std::string> slot;

		bool closed = false;

	public:
		// Waits for the slot to be free and puts a file in it.
		void put(std::string file);

		// Waits for a file and takes it out. Nothing is the relay closed and empty, which is the
		// only way the taking ends: a taker that stopped early would leave the asker waiting on a
		// slot nothing empties.
		std::optional<std::string> take();

		// No more files are coming.
		void close();
	};
}

#endif
