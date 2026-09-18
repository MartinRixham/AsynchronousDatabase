#include <algorithm>

#include "shared_pool.h"

namespace
{
	// Idle connections, not open ones: a request in flight holds one of its own whatever this is, and
	// requests here are not bounded by the threads serving them. This is how many a burst of them
	// leaves open to be used again.
	constexpr size_t connection_cache = 256;
}

boost::asio::execution_context::id http::shared_pool::id;

http::shared_pool::shared_pool(boost::asio::execution_context &context):
	boost::asio::execution_context::service(context)
{
}

std::unique_ptr<http::connection> http::shared_pool::take(
	const boost::asio::any_io_executor &executor,
	const std::string &host,
	const std::string &port)
{
	std::lock_guard<std::mutex> lock(mutex);

	auto held = std::ranges::find_if(
		idle,
		[&host, &port](const std::unique_ptr<connection> &kept) { return kept->goes_to(host, port); });

	if (held == idle.end())
	{
		return std::make_unique<connection>(executor, host, port);
	}

	std::unique_ptr<connection> taken = std::move(*held);

	idle.erase(held);

	return taken;
}

void http::shared_pool::keep(std::unique_ptr<connection> used)
{
	if (!used->is_open())
	{
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);

	if (idle.size() >= connection_cache)
	{
		idle.erase(idle.begin());
	}

	idle.push_back(std::move(used));
}

void http::shared_pool::shutdown()
{
	std::lock_guard<std::mutex> lock(mutex);

	idle.clear();
}
