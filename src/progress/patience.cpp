#include "patience.h"

progress::patience::patience(long seconds):
	without(seconds),
	expires(std::chrono::steady_clock::now() + without)
{
}

bool progress::patience::spent() const
{
	return std::chrono::steady_clock::now() >= expires;
}

void progress::patience::renew()
{
	expires = std::chrono::steady_clock::now() + without;
}
