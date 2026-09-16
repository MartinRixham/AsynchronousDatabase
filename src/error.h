#pragma once

#include <source_location>
#include <string>

inline std::string located(
	const std::string &message,
	const std::source_location &where = std::source_location::current())
{
	return std::string(where.file_name()) +
		":" +
		std::to_string(where.line()) +
		" " +
		where.function_name() +
		": " +
		message;
}
