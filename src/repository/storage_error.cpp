#include "storage_error.h"

repository::storage_error::storage_error(error::code code, const std::string &message):
	std::runtime_error(message),
	error_code(code)
{
}

error::code repository::storage_error::code() const
{
	return error_code;
}
