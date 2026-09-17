#include "location.h"

namespace
{
	constexpr char scheme[] = "http://";

	constexpr size_t scheme_length = sizeof(scheme) - 1;
}

http::location http::locate(const std::string &url)
{
	location where;

	if (url.compare(0, scheme_length, scheme) != 0)
	{
		return where;
	}

	size_t end = url.find_first_of("/?", scheme_length);

	where.authority = url.substr(scheme_length, end == std::string::npos ? std::string::npos : end - scheme_length);

	if (end == std::string::npos)
	{
		where.target = "/";
	}
	else
	{
		where.target = url[end] == '/' ? url.substr(end) : "/" + url.substr(end);
	}

	// An address in brackets is IPv6 and holds colons of its own, so what names the port is the
	// colon after the brackets rather than the last one.
	size_t bracket = where.authority.rfind(']');
	size_t colon = where.authority.rfind(':');

	if (colon != std::string::npos && (bracket == std::string::npos || colon > bracket))
	{
		where.host = where.authority.substr(0, colon);
		where.port = where.authority.substr(colon + 1);
	}
	else
	{
		where.host = where.authority;
		where.port = "80";
	}

	if (bracket != std::string::npos && where.host.size() > 1 && where.host.front() == '[')
	{
		where.host = where.host.substr(1, bracket - 1);
	}

	where.is_valid = !where.host.empty() && !where.port.empty();

	return where;
}
