#pragma once

#include <string>

#include "error/error_code.h"

namespace error
{
	struct error_message
	{
		error::code code {};

		std::string message;
	};
}
