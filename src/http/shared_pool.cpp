#include <algorithm>
#include <chrono>
#include <iterator>

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

	std::erase_if(
		idle,
		[&host, &port, now = std::chrono::steady_clock::now()](const std::unique_ptr<connection> &kept)
		{ return kept->goes_to(host, port) && !kept->is_reusable(now); });

	// The connection kept last is the one least likely to have been closed since.
	auto held = std::ranges::find_if(
		idle.rbegin(),
		idle.rend(),
		[&host, &port](const std::unique_ptr<connection> &kept) { return kept->goes_to(host, port); });

	if (held == idle.rend())
	{
		return std::make_unique<connection>(executor, host, port);
	}

	std::unique_ptr<connection> taken = std::move(*held);

	idle.erase(std::next(held).base());

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

	used->mark_idle();
	idle.push_back(std::move(used));
}

void http::shared_pool::shutdown()
{
	std::lock_guard<std::mutex> lock(mutex);

	idle.clear();
}
