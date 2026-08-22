#include <cstring>

#include "base64.h"

namespace
{
	constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string base64::encode(const std::string &text)
{
	std::string encoded;

	for (size_t i = 0; i < text.size(); i += 3)
	{
		size_t remaining = text.size() - i;
		unsigned int group = static_cast<unsigned char>(text[i]) << 16;

		group |= (remaining > 1 ? static_cast<unsigned char>(text[i + 1]) : 0) << 8;
		group |= remaining > 2 ? static_cast<unsigned char>(text[i + 2]) : 0;

		encoded += alphabet[(group >> 18) & 0x3f];
		encoded += alphabet[(group >> 12) & 0x3f];
		encoded += remaining > 1 ? alphabet[(group >> 6) & 0x3f] : '=';
		encoded += remaining > 2 ? alphabet[group & 0x3f] : '=';
	}

	return encoded;
}

bool base64::decode(const std::string &encoded, std::string *text)
{
	unsigned int group = 0;
	int bits = 0;

	text->clear();

	for (size_t i = 0; i < encoded.size(); i++)
	{
		if (encoded[i] == '=')
		{
			break;
		}

		const char *found = strchr(alphabet, encoded[i]);

		if (found == NULL || encoded[i] == '\0')
		{
			return false;
		}

		group = (group << 6) | static_cast<unsigned int>(found - alphabet);
		bits += 6;

		if (bits >= 8)
		{
			bits -= 8;
			*text += static_cast<char>((group >> bits) & 0xff);
		}
	}

	return true;
}
