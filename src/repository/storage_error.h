#ifndef REPOSITORY_STORAGE_ERROR_H
#define REPOSITORY_STORAGE_ERROR_H

#include <stdexcept>
#include <string>

namespace repository
{
	// Genuine infrastructure failure, as opposed to a validation failure, which is a value. The
	// code is one of the documented error codes, so that back pressure is told from a real error.
	class storage_error : public std::runtime_error
	{
		std::string error_code;

	public:
		storage_error(const std::string &code, const std::string &message);

		const std::string &code() const;
	};
}

#endif
