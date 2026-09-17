#pragma once

#include <stdexcept>
#include <string>

#include "error/error_code.h"

namespace repository
{
	// Genuine infrastructure failure, as opposed to a validation failure, which is a value. The
	// code is one of the documented error codes, so that back pressure is told from a real error.
	class storage_error : public std::runtime_error
	{
		error::code error_code;

	public:
		storage_error(error::code code, const std::string &message);

		error::code code() const;
	};
}
