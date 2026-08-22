#include "repository.h"

repository::storage_error::storage_error(const std::string &code, const std::string &message):
	std::runtime_error(message),
	error_code(code)
{
}

const std::string &repository::storage_error::code() const
{
	return error_code;
}
