#ifndef LOG_H
#define LOG_H

#include <iostream>

#define LEVEL_DEBUG 1

#if LOG == LEVEL_DEBUG
	#define DEBUG(message) std::cerr << "DEBUG: " << message << std::endl;
#else
	#define DEBUG(message)
#endif

#endif
