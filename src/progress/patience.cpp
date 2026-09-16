#include "patience.h"

progress::patience::patience(long seconds) noexcept:
	without(seconds),
	expires((std::chrono::steady_clock::now() + without).time_since_epoch().count())
{
}

bool progress::patience::spent() const noexcept
{
	return std::chrono::steady_clock::now().time_since_epoch().count() >= expires.load();
}

void progress::patience::renew() noexcept
{
	expires.store((std::chrono::steady_clock::now() + without).time_since_epoch().count());
}
