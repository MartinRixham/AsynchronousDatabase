#include <algorithm>
#include <chrono>
#include <iterator>

#include "pool.h"

namespace
{
	// Connections one thread keeps. A thread talks to every node it forwards to and holds one
	// connection to each, so a cache smaller than the cluster is a handshake on every forward
	// past the last node it used. It is a ceiling and not a reservation.
	constexpr size_t connection_cache = 64;
}

boost::asio::io_context &http::pool::context() noexcept
{
	return io;
}

std::unique_ptr<http::connection> http::pool::take(const std::string &host, const std::string &port)
{
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
		return std::make_unique<connection>(io.get_executor(), host, port);
	}

	std::unique_ptr<connection> taken = std::move(*held);

	idle.erase(std::next(held).base());

	return taken;
}

void http::pool::keep(std::unique_ptr<connection> used)
{
	if (!used->is_open())
	{
		return;
	}

	// The connection that has gone longest without being asked for is the one a cluster larger
	// than the cache can most afford to lose.
	if (idle.size() >= connection_cache)
	{
		idle.erase(idle.begin());
	}

	used->mark_idle();
	idle.push_back(std::move(used));
}

void http::pool::run()
{
	io.restart();
	io.run();
}
