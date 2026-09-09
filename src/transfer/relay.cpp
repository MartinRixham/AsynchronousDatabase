#include <utility>

#include "relay.h"

void transfer::relay::put(std::string file)
{
	std::unique_lock<std::mutex> lock(mutex);

	emptied.wait(lock, [this]() { return !slot.has_value(); });

	slot = std::move(file);

	lock.unlock();
	filled.notify_one();
}

std::optional<std::string> transfer::relay::take()
{
	std::unique_lock<std::mutex> lock(mutex);

	filled.wait(lock, [this]() { return slot.has_value() || closed; });

	if (!slot)
	{
		return std::nullopt;
	}

	std::optional<std::string> file = std::move(slot);

	slot.reset();

	lock.unlock();
	emptied.notify_one();

	return file;
}

void transfer::relay::close()
{
	{
		std::lock_guard<std::mutex> lock(mutex);

		closed = true;
	}

	filled.notify_all();
}
