#ifndef BASE64_BASE64_H
#define BASE64_BASE64_H

#include <string>

namespace base64
{
	std::string encode(const std::string &text);

	bool decode(const std::string &encoded, std::string *text);
}

#endif
