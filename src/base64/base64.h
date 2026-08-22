#ifndef BASE64_BASE64_H
#define BASE64_BASE64_H

#include <string>

namespace base64
{
	// Standard alphabet with padding, which is what a scan cursor carries and what etcd's JSON
	// gateway demands of every key and value.
	std::string encode(const std::string &text);

	// False when the text is not base64 at all, which is a cursor a client made up rather than
	// one this instance issued.
	bool decode(const std::string &encoded, std::string *text);
}

#endif
