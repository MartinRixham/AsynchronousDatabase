#pragma once

#import "error/error_code.h"

struct error_message
{
	error::code code {};

	std::string message;
};
