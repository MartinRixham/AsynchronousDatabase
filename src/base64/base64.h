#pragma once

#include <optional>
#include <string>

namespace base64
{
	std::string encode(const std::string &text);

	std::optional<std::string> decode(const std::string &encoded);
}
